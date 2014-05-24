/*
 * Copyright (C) 2000, The University of Queensland
 * Copyright (C) 2001, Sun Microsystems, Inc
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 *
 */

/** \file PalmBinaryFile.cpp
 * This class loads a Palm Pilot .prc file. Derived from class BinaryFile
 */

/*
 * 02 Feb 00 - Mike: Initial version
 * 24 Feb 00 - Mike: Support for system trapcalls
 * 17 Apr 00 - Mike: GetAppID(); find PilotMain with patterns
 * 16 Feb 01 - Nathan: removed util references
 * 01 Aug 01 - Mike: Changed GetGlobalPointerInfo to the new definition
 */

#include "PalmBinaryFile.h"
#include "palmsystraps.h"

#include <cassert>
#include <cstring>
#include <cstdlib>

// Macro to convert a pointer to a Big Endian integer into a host integer
#define UC(ad) ((unsigned char*)ad)
#define UINT4(p) ((UC((p))[0] << 24) + (UC(p)[1] << 16) + (UC(p)[2] << 8) + \
    UC(p)[3])
#define UINT4ADDR(p) ((UC((p).m_value)[0] << 24) + (UC((p).m_value)[1] << 16) + (UC((p).m_value)[2] << 8) + \
    UC((p).m_value)[3])

PalmBinaryFile::PalmBinaryFile()
    : m_pImage(nullptr), m_pData(nullptr) {
}

PalmBinaryFile::~PalmBinaryFile() {
    for (int i=0; i < m_iNumSections; i++)
        if (m_pSections[i].pSectionName != nullptr)
            delete [] m_pSections[i].pSectionName;
    if (m_pImage) {
        delete [] m_pImage;
    }
    if (m_pData) {
        delete [] m_pData;
    }
}
static int Read2(short* ps) {
    unsigned char* p = (unsigned char*)ps;
    // Little endian
    int n = (int(p[0])<<8) | p[1];
    return n;
}

int Read4(int* pi) {
    short* p = (short*)pi;
    int n1 = Read2(p);
    int n2 = Read2(p+1);
    int n = (int) ((n1<<16) | n2);
    return n;
}
// Read 1 byte from given native address
char PalmBinaryFile::readNative1(ADDRESS nat) {
    SectionInfo *si = GetSectionInfoByAddr(nat);
    if (si == nullptr)
        si = GetSectionInfo(0);
    char* host = (char*)(si->uHostAddr - si->uNativeAddr + nat).m_value;
    return *host;
}

// Read 2 bytes from given native address
int PalmBinaryFile::readNative2(ADDRESS nat) {
    SectionInfo *si = GetSectionInfoByAddr(nat);
    if (si == nullptr) return 0;
    ADDRESS host = si->uHostAddr - si->uNativeAddr + nat;
    int n = Read2((short*)host.m_value);
    return n;
}

// Read 4 bytes from given native address
int PalmBinaryFile::readNative4(ADDRESS nat) {
    SectionInfo *si = GetSectionInfoByAddr(nat);
    if (si == nullptr) return 0;
    ADDRESS host = si->uHostAddr - si->uNativeAddr + nat;
    int n = Read4((int*)host.m_value);
    return n;
}
// Read 8 bytes from given native address
QWord PalmBinaryFile::readNative8(ADDRESS nat) { //TODO: lifted from Win32 loader, likely wrong
    int raw[2];
#ifdef WORDS_BIGENDIAN        // This tests the host machine
    // Source and host are different endianness
    raw[1] = readNative4(nat);
    raw[0] = readNative4(nat+4);
#else
    // Source and host are same endianness
    raw[0] = readNative4(nat);
    raw[1] = readNative4(nat+4);
#endif
    return *(QWord*)raw;
}

// Read 4 bytes as a float
float PalmBinaryFile::readNativeFloat4(ADDRESS nat) {
    int raw = readNative4(nat);
    // Ugh! gcc says that reinterpreting from int to float is invalid!!
    //return reinterpret_cast<float>(raw);        // Note: cast, not convert!!
    return *(float*)&raw;                        // Note: cast, not convert
}

// Read 8 bytes as a float
double PalmBinaryFile::readNativeFloat8(ADDRESS nat) { //TODO: lifted from Win32 loader, likely wrong
    int raw[2];
#ifdef WORDS_BIGENDIAN        // This tests the host machine
    // Source and host are different endianness
    raw[1] = readNative4(nat);
    raw[0] = readNative4(nat+4);
#else
    // Source and host are same endianness
    raw[0] = readNative4(nat);
    raw[1] = readNative4(nat+4);
#endif
    //return reinterpret_cast<double>(*raw);    // Note: cast, not convert!!
    return *(double*)raw;
}

bool PalmBinaryFile::RealLoad(const char* sName) {
    FILE    *fp;
    char    buf[32];

    m_pFileName = sName;

    if ((fp = fopen(sName, "rb")) == nullptr) {
        fprintf(stderr, "Could not open binary file %s\n", sName);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);

    // Allocate a buffer for the image
    m_pImage = new unsigned char[size];
    if (m_pImage == nullptr) {
        fprintf(stderr, "Could not allocate %ld bytes for image\n", size);
        return false;
    }
    memset(m_pImage, size, 0);

    fseek(fp, 0, SEEK_SET);
    if (fread(m_pImage, 1, size, fp) != (unsigned)size) {
        fprintf(stderr, "Error reading binary file %s\n", sName);
        return false;
    }

    // Check type at offset 0x3C; should be "appl" (or "palm"; ugh!)
    if ((strncmp((char*)(m_pImage+0x3C), "appl", 4) != 0) &&
            (strncmp((char*)(m_pImage+0x3C), "panl", 4) != 0) &&
            (strncmp((char*)(m_pImage+0x3C), "libr", 4) != 0)) {
        fprintf(stderr, "%s is not a standard .prc file\n", sName);
        return false;
    }

    // Get the number of resource headers (one section per resource)
    m_iNumSections = (m_pImage[0x4C] << 8) + m_pImage[0x4D];

    // Allocate the section information
    m_pSections = new SectionInfo[m_iNumSections];
    if (m_pSections == nullptr) {
        fprintf(stderr, "Could not allocate section info array of %d items\n",
                m_iNumSections);
        if (m_pImage) {
            delete m_pImage;
            m_pImage = nullptr;
        }
    }

    // Iterate through the resource headers (generating section info structs)
    unsigned char* p = m_pImage + 0x4E;          // First resource header
    unsigned off = 0;
    for (int i=0; i < m_iNumSections; i++) {
        // First get the name (4 alpha)
        strncpy(buf, (char*)p, 4);
        buf[4] = '\0';
        std::string name(buf);
        // Now get the identifier (2 byte binary)
        unsigned id = (p[4] << 8) + p[5];
        sprintf(buf, "%d", id);
        // Join the id to the name, e.g. code0, data12
        name += buf;
        m_pSections[i].pSectionName = new char[name.size()+1];
        strcpy(m_pSections[i].pSectionName, name.c_str());
        p += 4+2;
        off = UINT4(p);
        p += 4;
        m_pSections[i].uNativeAddr = off;
        m_pSections[i].uHostAddr = ADDRESS::host_ptr(m_pImage+off);

        // Guess the length
        if (i > 0) {
            m_pSections[i-1].uSectionSize = off - m_pSections[i-1].uNativeAddr.m_value;
            m_pSections[i].uSectionEntrySize = 1;        // No info available
        }

        // Decide if code or data; note that code0 is a special case (not code)
        m_pSections[i].bCode =
                (name != "code0") && (name.substr(0, 4) == "code");
        m_pSections[i].bData = name.substr(0, 4) == "data";

    }

    // Set the length for the last section
    m_pSections[m_iNumSections-1].uSectionSize = size - off;

    // Create a separate, uncompressed, initialised data section
    SectionInfo* pData = GetSectionInfoByName("data0");
    if (pData == nullptr) {
        fprintf(stderr, "No data section!\n");
        return false;
    }

    SectionInfo* pCode0 = GetSectionInfoByName("code0");
    if (pCode0 == nullptr) {
        fprintf(stderr, "No code 0 section!\n");
        return false;
    }

    // When the info is all boiled down, the two things we need from the
    // code 0 section are at offset 0, the size of data above a5, and at
    // offset 4, the size below. Save the size below as a member variable
    m_SizeBelowA5 = UINT4ADDR(pCode0->uHostAddr+4);
    // Total size is this plus the amount above (>=) a5
    unsigned sizeData = m_SizeBelowA5 + UINT4ADDR(pCode0->uHostAddr);

    // Allocate a new data section
    m_pData = new unsigned char[sizeData];
    if (m_pData == nullptr) {
        fprintf(stderr, "Could not allocate %u bytes for data section\n",
                sizeData);
    }

    // Uncompress the data. Skip first long (offset of CODE1 "xrefs")
    p = (unsigned char*)(pData->uHostAddr+4).m_value;
    int start = (int) UINT4(p);
    p += 4;
    unsigned char* q = (m_pData + m_SizeBelowA5 + start);
    bool done = false;
    while (!done && (p < (unsigned char*)(pData->uHostAddr +
                                          pData->uSectionSize).m_value)) {
        unsigned char rle = *p++;
        if (rle == 0) {
            done = true;
            break;
        }
        else if (rle == 1) {
            // 0x01 b_0 b_1
            // => 0x00 0x00 0x00 0x00 0xFF 0xFF b_0 b_1
            *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0;
            *q++ = 0xFF; *q++ = 0xFF; *q++ = *p++; *q++ = *p++;
        }
        else if (rle == 2) {
            // 0x02 b_0 b_1 b_2
            // => 0x00 0x00 0x00 0x00 0xFF b_0 b_1 b_2
            *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0;
            *q++ = 0xFF; *q++ = *p++; *q++ = *p++; *q++ = *p++;
        }
        else if (rle == 3) {
            // 0x03 b_0 b_1 b_2
            // => 0xA9 0xF0 0x00 0x00 b_0 b_1 0x00 b_2
            *q++ = 0xA9; *q++ = 0xF0; *q++ = 0; *q++ = 0;
            *q++ = *p++; *q++ = *p++; *q++ = 0; *q++ = *p++;
        }
        else if (rle == 4) {
            // 0x04 b_0 b_1 b_2 b_3
            // => 0xA9 axF0 0x00 b_0 b_1 b_3 0x00 b_3
            *q++ = 0xA9; *q++ = 0xF0; *q++ = 0; *q++ = *p++;
            *q++ = *p++; *q++ = *p++; *q++ = 0; *q++ = *p++;
        }
        else if (rle < 0x10) {
            // 5-0xF are invalid.
            assert(0);
        }
        else if (rle >= 0x80) {
            // n+1 bytes of literal data
            for (int k=0; k <= (rle-0x80); k++)
                *q++ = *p++;
        }
        else if (rle >= 40) {
            // n+1 repetitions of 0
            for (int k=0; k <= (rle-0x40); k++)
                *q++ = 0;
        }
        else if (rle >= 20) {
            // n+2 repetitions of b
            unsigned char b = *p++;
            for (int k=0; k < (rle-0x20+2); k++)
                *q++ = b;
        }
        else {
            // 0x10: n+1 repetitions of 0xFF
            for (int k=0; k <= (rle-0x10); k++)
                *q++ = 0xFF;
        }
    }

    if (!done)
        fprintf(stderr, "Warning! Compressed data section premature end\n");
    //printf("Used %u bytes of %u in decompressing data section\n",
    //p-(unsigned char*)pData->uHostAddr, pData->uSectionSize);

    // Replace the data pointer and size with the uncompressed versions
    pData->uHostAddr = ADDRESS::host_ptr(m_pData);
    pData->uSectionSize = sizeData;
    // May as well make the native address zero; certainly the offset in the
    // file is no longer appropriate (and is confusing)
    pData->uNativeAddr = 0;

    return true;
}

void PalmBinaryFile::UnLoad() {
    if (m_pImage) {
        delete [] m_pImage;
        m_pImage = nullptr;
    }
}

// This is provided for completeness only...
std::list<SectionInfo*>& PalmBinaryFile::GetEntryPoints(const char* pEntry
                                                        /* = "main" */) {
    std::list<SectionInfo*>* ret = new std::list<SectionInfo*>;
    SectionInfo* pSect = GetSectionInfoByName("code1");
    if (pSect == nullptr)
        return *ret;               // Failed
    ret->push_back(pSect);
    return *ret;
}

ADDRESS PalmBinaryFile::GetEntryPoint() {
    assert(0); /* FIXME: Need to be implemented */
    return ADDRESS::g(0L);
}

bool PalmBinaryFile::Open(const char* sName) {
    // Not implemented yet
    return false;
}
void PalmBinaryFile::Close() {
    // Not implemented yet
    return;
}
bool PalmBinaryFile::PostLoad(void* handle) {
    // Not needed: for archives only
    return false;
}

LOAD_FMT PalmBinaryFile::GetFormat() const {
    return LOADFMT_PALM;
}

MACHINE PalmBinaryFile::GetMachine() const {
    return MACHINE_PALM;
}

bool PalmBinaryFile::isLibrary() const {
    return (strncmp((char*)(m_pImage+0x3C), "libr", 4) == 0);
}
std::list<const char *> PalmBinaryFile::getDependencyList() {
    return std::list<const char *>(); /* doesn't really exist on palm */
}

ADDRESS PalmBinaryFile::getImageBase() {
    return ADDRESS::g(0L); /* FIXME */
}

size_t PalmBinaryFile::getImageSize() {
    return 0; /* FIXME */
}

// We at least need to be able to name the main function and system calls
const char* PalmBinaryFile::SymbolByAddress(ADDRESS dwAddr) {
    if ((dwAddr.m_value & 0xFFFFF000) == 0xAAAAA000) {
        // This is the convention used to indicate an A-line system call
        unsigned offset = dwAddr.m_value & 0xFFF;
        if (offset < numTrapStrings)
            return trapNames[offset];
        else
            return nullptr;
    }
    if (dwAddr == GetMainEntryPoint())
        return "PilotMain";
    else return nullptr;
}

// Not really dynamically linked, but the closest thing
bool PalmBinaryFile::IsDynamicLinkedProc(ADDRESS uNative) {
    return ((uNative.m_value & 0xFFFFF000) == 0xAAAAA000);
}

// Specific to BinaryFile objects that implement a "global pointer"
// Gets a pair of unsigned integers representing the address of %agp,
// and the value for GLOBALOFFSET. For Palm, the latter is the amount of
// space allocated below %a5, i.e. the difference between %a5 and %agp
// (%agp points to the bottom of the global data area).
std::pair<ADDRESS,unsigned> PalmBinaryFile::GetGlobalPointerInfo() {
    ADDRESS agp = ADDRESS::g(0L);
    const SectionInfo* ps = GetSectionInfoByName("data0");
    if (ps) agp = ps->uNativeAddr;
    std::pair<ADDRESS, unsigned> ret(agp, m_SizeBelowA5);
    return ret;
}

//  //  //  //  //  //  //
//  Specific for Palm   //
//  //  //  //  //  //  //

int PalmBinaryFile::GetAppID() const {
    // The answer is in the header. Return 0 if file not loaded
    if (m_pImage == nullptr)
        return 0;
    // Beware the endianness (large)
#define OFFSET_ID 0x40
    return (m_pImage[OFFSET_ID  ] << 24) + (m_pImage[OFFSET_ID+1] << 16) +
            (m_pImage[OFFSET_ID+2] <<  8) + (m_pImage[OFFSET_ID+3]);
}

// Patterns for Code Warrior
#define WILD 0x4AFC
static SWord CWFirstJump[] = {
    0x0, 0x1,           // ? All Pilot programs seem to start with this
    0x487a, 0x4,        // pea 4(pc)
    0x0697, WILD, WILD, // addil #number, (a7)
    0x4e75};            // rts
static SWord CWCallMain[] = {
    0x487a, 14,         // pea 14(pc)
    0x487a, 4,          // pea 4(pc)
    0x0697, WILD, WILD, // addil #number, (a7)
    0x4e75};            // rts
static SWord GccCallMain[] = {
    0x3F04,             // movew d4, -(a7)
    0x6100, WILD,       // bsr xxxx
    0x3F04,             // movew d4, -(a7)
    0x2F05,             // movel d5, -(a7)
    0x3F06,             // movew d6, -(a7)
    0x6100, WILD};      // bsr PilotMain

/***************************************************************************//**
 *
 * \brief      Try to find a pattern
 * PARAMETERS:    start - pointer to code to start searching
 *                patt - pattern to look for
 *                pattSize - size of the pattern (in SWords)
 *                max - max number of SWords to search
 * \returns       0 if no match; pointer to start of match if found
 ******************************************************************************/
SWord* findPattern(SWord* start, const SWord* patt, int pattSize, int max) {
    const SWord* last = start + max;
    for (; start < last; start++) {
        bool found = true;
        for (int i=0; i < pattSize; i++) {
            SWord curr = patt[i];
            if ((curr != WILD) && (curr != start[i])) {
                found = false;
                break;              // Mismatch
            }
        }
        if (found)
            // All parts of the pattern matched
            return start;
    }
    // Each start position failed
    return nullptr;
}

// Find the native address for the start of the main entry function.
// For Palm binaries, this is PilotMain.
ADDRESS PalmBinaryFile::GetMainEntryPoint() {
    SectionInfo* pSect = GetSectionInfoByName("code1");
    if (pSect == nullptr)
        return ADDRESS::g(0L);               // Failed
    // Return the start of the code1 section
    SWord* startCode = (SWord*) pSect->uHostAddr.m_value;
    int delta = (pSect->uHostAddr - pSect->uNativeAddr).m_value;

    // First try the CW first jump pattern
    SWord* res = findPattern(startCode, CWFirstJump,
                             sizeof(CWFirstJump) / sizeof(SWord), 1);
    if (res) {
        // We have the code warrior first jump. Get the addil operand
        int addilOp = (startCode[5] << 16) + startCode[6];
        SWord* startupCode = (SWord*)(ADDRESS::host_ptr(startCode) + 10 + addilOp).m_value;
        // Now check the next 60 SWords for the call to PilotMain
        res = findPattern(startupCode, CWCallMain,
                          sizeof(CWCallMain) / sizeof(SWord), 60);
        if (res) {
            // Get the addil operand
            addilOp = (res[5] << 16) + res[6];
            // That operand plus the address of that operand is PilotMain
            return ADDRESS::host_ptr(res) + 10 + addilOp - delta;
        }
        else {
            fprintf( stderr, "Could not find call to PilotMain in CW app\n" );
            return ADDRESS::g(0L);
        }
    }
    // Check for gcc call to main
    res = findPattern(startCode, GccCallMain,
                      sizeof(GccCallMain) / sizeof(SWord), 75);
    if (res) {
        // Get the operand to the bsr
        SWord bsrOp = res[7];
        return ADDRESS::host_ptr(res) + 14 + bsrOp - delta;
    }

    fprintf(stderr,"Cannot find call to PilotMain\n");
    return ADDRESS::g(0L);
}

void PalmBinaryFile::GenerateBinFiles(const std::string& path) const {
    for (int i=0; i < m_iNumSections; i++) {
        SectionInfo* pSect = m_pSections + i;
        if ((strncmp(pSect->pSectionName, "code", 4) != 0) &&
                (strncmp(pSect->pSectionName, "data", 4) != 0)) {
            // Save this section in a file
            // First construct the file name
            char name[20];
            strncpy(name, pSect->pSectionName, 4);
            sprintf(name+4, "%04x.bin", atoi(pSect->pSectionName+4));
            std::string fullName(path);
            fullName += name;
            // Create the file
            FILE* f = fopen(fullName.c_str(), "w");
            if (f == nullptr) {
                fprintf( stderr, "Could not open %s for writing binary file\n",
                         fullName.c_str() );
                return;
            }
            fwrite((void*)pSect->uHostAddr.m_value, pSect->uSectionSize, 1, f);
            fclose(f);
        }
    }
}

// This function is called via dlopen/dlsym; it returns a Binary::getFile
// derived concrete object. After this object is returned, the virtual function
// call mechanism will call the rest of the code in this library
// It needs to be C linkage so that it its name is not mangled
extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#endif
BinaryFile* construct() {
    return new PalmBinaryFile;
}
}