#include "Ppmd8.h"
#include <stdint.h>

static void* pmalloc(ISzAllocPtr ip, size_t size)
{
    (void)ip;
    return malloc(size);
}

static void pfree(ISzAllocPtr ip, void* addr)
{
    (void)ip;
    free(addr);
}

struct CharWriter
{
    IByteOut streamOut;
    char* ptr;
    char* end;
};

static void charWriterFunc(const IByteOut* p, Byte b)
{
    struct CharWriter* cw = (struct CharWriter*)p;
    if (cw->ptr < cw->end)
        *cw->ptr = (char)b;
    cw->ptr++;
}

struct CharReader
{
    IByteIn streamIn;
    const char* ptr;
    const char* end;
};

static uint8_t charReaderFunc(const IByteIn* p)
{
    struct CharReader* cr = (struct CharReader*)p;
    if (cr->ptr < cr->end)
        return (uint8_t)*cr->ptr++;
    cr->ptr++;
    return 0;
}

size_t ppmd_compress(void* dst, size_t dstCapacity,
    const void* src, size_t srcSize, int compressionLevel)
{
    CPpmd8 ppmd;
    struct CharWriter cw;
    unsigned char* ubody;
    unsigned modelOrder, memMb, restoreMethod, wPPMd;
    ISzAlloc ialloc = {pmalloc, pfree};

    if (dstCapacity < 2 || compressionLevel < 0 || compressionLevel > 9)
        return 0;

    if (compressionLevel == 0)
        compressionLevel = 1;
    memMb = 1 << (compressionLevel - 1);
    modelOrder = 3 + compressionLevel;
    restoreMethod = compressionLevel < 7 ?
        PPMD8_RESTORE_METHOD_RESTART : PPMD8_RESTORE_METHOD_CUT_OFF;
    wPPMd = (modelOrder - 1) + ((memMb - 1) << 4) + (restoreMethod << 12);

    cw.streamOut.Write = &charWriterFunc;
    cw.ptr = (char*)dst;
    cw.end = cw.ptr + dstCapacity;
    ppmd.Stream.Out = &cw.streamOut;

    Ppmd8_Construct(&ppmd);
    if (!Ppmd8_Alloc(&ppmd, memMb << 20, &ialloc))
        return 0;
    Ppmd8_Init_RangeEnc(&ppmd);
    Ppmd8_Init(&ppmd, modelOrder, restoreMethod);

    ppmd.Stream.Out->Write(&cw.streamOut, (Byte)(wPPMd & 0xff));
    ppmd.Stream.Out->Write(&cw.streamOut, (Byte)(wPPMd >> 8));

    ubody = (unsigned char*)src;
    for (size_t i = 0; i < srcSize; ++i)
        Ppmd8_EncodeSymbol(&ppmd, ubody[i]);
    Ppmd8_EncodeSymbol(&ppmd, -1); /* EndMark */
    Ppmd8_Flush_RangeEnc(&ppmd);
    Ppmd8_Free(&ppmd, &ialloc);
    if (cw.ptr > cw.end) // overflow
        return 0;
    return cw.ptr - (char*)dst;
}

size_t ppmd_decompress(void* dst, size_t dstCapacity,
    const void* src, size_t compressedSize)
{
    CPpmd8 ppmd;
    struct CharReader cr;
    unsigned b0, b1, wPPMd, modelOrder, memMb, restoreMethod, ret;
    char *ptr, *ptrEnd;
    ISzAlloc ialloc = {pmalloc, pfree};
    if (compressedSize < 2)
        return 0;

    cr.streamIn.Read = &charReaderFunc;
    cr.ptr = (const char*)src;
    cr.end = cr.ptr + compressedSize;

    b0 = charReaderFunc(&cr.streamIn);
    b1 = charReaderFunc(&cr.streamIn);
    wPPMd = b0 | (b1 << 8);
    modelOrder = (wPPMd & 0xf) + 1;
    memMb = ((wPPMd >> 4) & 0xff) + 1;
    restoreMethod = wPPMd >> 12;

    if (modelOrder < 2 || restoreMethod > 1)
        return 0;
    ppmd.Stream.In = &cr.streamIn;

    Ppmd8_Construct(&ppmd);
    if (!Ppmd8_Alloc(&ppmd, memMb << 20, &ialloc))
        return 0;
    if (!Ppmd8_Init_RangeDec(&ppmd))
    {
        Ppmd8_Free(&ppmd, &ialloc);
        return 0;
    }
    Ppmd8_Init(&ppmd, modelOrder, restoreMethod);

    ptr = (char*)dst;
    ptrEnd = ptr + dstCapacity;
    while (ptr <= ptrEnd)
    {
        int c = Ppmd8_DecodeSymbol(&ppmd);
        if (cr.ptr > cr.end || c < 0)
            break;
        if (ptr < ptrEnd)
            *ptr = (char)(unsigned)c;
        ptr++;
    }
    ret = Ppmd8_RangeDec_IsFinishedOK(&ppmd) && cr.ptr >= cr.end ? 0 : -1;
    Ppmd8_Free(&ppmd, &ialloc);
    if (cr.ptr > cr.end || ret == -1)
        return 0;
    return ptr - (char*)dst;
}
