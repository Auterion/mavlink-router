/*
 * This file is part of the MAVLink Router project
 *
 * Per-endpoint ZSTD compression for UDP endpoints, to save bandwidth on a
 * constrained link (e.g. a telemetry radio):
 *   - each outgoing UDP datagram (one or more already-framed MAVLink messages,
 *     possibly coalesced) is compressed into a single standalone ZSTD frame,
 *     using a trained dictionary at level 3;
 *   - a datagram whose first message is in a time-critical skip-list, or that
 *     does not get smaller, is sent uncompressed instead;
 *   - the receiver auto-detects the 4-byte ZSTD magic to decide whether to
 *     decompress the datagram or pass it through unchanged.
 *
 * The ZSTD library types are kept out of this header (opaque void*) so that
 * <zstd.h> does not leak into the rest of the tree.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

class ZstdCodec {
public:
    ZstdCodec() = default;
    ~ZstdCodec();
    ZstdCodec(const ZstdCodec &) = delete;
    ZstdCodec &operator=(const ZstdCodec &) = delete;

    /**
     * Build the compression/decompression contexts. @p dict_path may be empty
     * (compress without a dictionary). Returns false only on allocation
     * failure; a missing/invalid dictionary file logs a warning and falls back
     * to dictionary-less operation.
     */
    bool init(const std::string &dict_path, int level = 3);

    /**
     * Compress one outgoing datagram. On success returns a pointer to an
     * internal buffer holding the compressed frame and sets *out_len. Returns
     * nullptr when the caller should transmit the original @p in bytes
     * unchanged (skip-listed first message id, compression did not shrink the
     * data, or a compression error).
     */
    const uint8_t *compress(const uint8_t *in, size_t in_len, size_t *out_len);

    /**
     * Decompress one received datagram in place. If @p buf does not start with
     * the ZSTD magic it is left untouched (plain MAVLink pass-through) and
     * @p in_len is returned. Otherwise it is decompressed into @p buf (capacity
     * @p cap) and the decompressed length is returned. Returns -1 on a
     * decompression error or if the result would not fit (@p buf left unchanged;
     * caller should drop the datagram).
     */
    ssize_t inflate(uint8_t *buf, size_t in_len, size_t cap);

    /** True if @p p begins with the 4-byte little-endian ZSTD frame magic. */
    static bool looks_compressed(const uint8_t *p, size_t len);

private:
    void *_cctx = nullptr;  // ZSTD_CCtx*
    void *_dctx = nullptr;  // ZSTD_DCtx*
    void *_cdict = nullptr; // ZSTD_CDict*
    void *_ddict = nullptr; // ZSTD_DDict*
    std::vector<uint8_t> _dict;
    std::vector<uint8_t> _cbuf; // compression output scratch
    std::vector<uint8_t> _dbuf; // decompression output scratch
    int _level = 3;
};
