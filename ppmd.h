#ifndef LIBPPMD_H_
#define LIBPPMD_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*! ppmd_compress() :
 *  Compresses `src` into already allocated `dst` using ppmd-I compressor.
 *  NOTE: Providing `dstCapacity >= ppmd_compressBound(srcSize)` hints
 *        that ppmd should have enough space to successfully compress src data.
 *  @return : compressed size written into `dst` (<= `dstCapacity),
 *            or 0 if failed or not enough dstCapacity. */
size_t ppmd_compress(void* dst, size_t dstCapacity,
	const void* src, size_t srcSize, int compressionLevel);

/*! ppmd_decompress() :
 * `compressedSize` : must be the exact size of compressed data.
 * `dstCapacity` is an upper bound of originalSize to regenerate.
 * @return : the number of bytes decompressed into `dst` (<= `dstCapacity`),
 *           or 0 if decoding fails or not enough dstCapacity. */
size_t ppmd_decompress(void* dst, size_t dstCapacity,
	const void* src, size_t compressedSize);

#define PPMD_MAX_INPUT_SIZE ((sizeof(size_t)==8) ? 0xFF00FF00FF00FF00ULL : 0xFF00FF00U)
#define PPMD_COMPRESSBOUND(srcSize)   (((size_t)(srcSize) >= PPMD_MAX_INPUT_SIZE) ? 0 : (srcSize) + ((srcSize)>>8) + (((srcSize) < (128<<10)) ? (((128<<10) - (srcSize)) >> 11) : 0))
static size_t ppmd_compressBound(size_t srcSize) /*!< maximum compressed size in worst case scenario or 0 if srcSize is larger than PPMD_MAX_INPUT_SIZE */
{
	return PPMD_COMPRESSBOUND(srcSize);
}

#ifdef __cplusplus
}
#endif

#endif /* LIBPPMD_H_ */
