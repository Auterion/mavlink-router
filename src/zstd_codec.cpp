/*
 * This file is part of the MAVLink Router project
 *
 * Per-endpoint ZSTD compression for UDP endpoints. See zstd_codec.h.
 *
 * The whole implementation is compiled only when libzstd is available at build
 * time (HAVE_ZSTD, set by meson). Without it, a stub is compiled: init() fails
 * with a clear message, so an endpoint that requests ZstdCompression refuses to
 * start rather than silently mishandling traffic.
 */
#include "zstd_codec.h"

#include <common/log.h>

bool ZstdCodec::looks_compressed(const uint8_t *p, size_t len)
{
    // ZSTD frame magic 0xFD2FB528, little-endian on the wire.
    return len >= 4 && p[0] == 0x28 && p[1] == 0xB5 && p[2] == 0x2F && p[3] == 0xFD;
}

#if HAVE_ZSTD

#    include <cstring>
#    include <fstream>

#    include <zstd.h>

namespace {

/*
 * Time-critical / poorly-compressing message ids that are always sent
 * uncompressed.
 */
bool is_incompressible_msg_id(uint32_t id)
{
    switch (id) {
    case 0:   // HEARTBEAT
    case 2:   // SYSTEM_TIME
    case 4:   // PING
    case 42:  // MISSION_CURRENT
    case 76:  // COMMAND_LONG
    case 77:  // COMMAND_ACK
    case 111: // TIMESYNC
    case 411: // CURRENT_EVENT_SEQUENCE
    case 412: // REQUEST_EVENT
        return true;
    default:
        return false;
    }
}

/*
 * Parse the MAVLink message id of the first message in @p p (v1 or v2). Returns
 * false when the header is too short to read the id.
 */
bool first_msg_id(const uint8_t *p, size_t len, uint32_t *id)
{
    if (len < 1) {
        return false;
    }
    if (p[0] == 0xFE) { // MAVLink v1: msgid is one byte at offset 5
        if (len < 6) {
            return false;
        }
        *id = p[5];
        return true;
    }
    if (p[0] == 0xFD) { // MAVLink v2: msgid is 3 bytes LE at offset 7..9
        if (len < 10) {
            return false;
        }
        *id = (uint32_t)p[7] | ((uint32_t)p[8] << 8) | ((uint32_t)p[9] << 16);
        return true;
    }
    return false;
}

constexpr size_t kMaxPacket = 64 * 1024;

} // namespace

ZstdCodec::~ZstdCodec()
{
    if (_cdict) {
        ZSTD_freeCDict((ZSTD_CDict *)_cdict);
    }
    if (_ddict) {
        ZSTD_freeDDict((ZSTD_DDict *)_ddict);
    }
    if (_cctx) {
        ZSTD_freeCCtx((ZSTD_CCtx *)_cctx);
    }
    if (_dctx) {
        ZSTD_freeDCtx((ZSTD_DCtx *)_dctx);
    }
}

bool ZstdCodec::init(const std::string &dict_path, int level)
{
    _level = level;

    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (cctx == nullptr || dctx == nullptr) {
        if (cctx != nullptr) {
            ZSTD_freeCCtx(cctx);
        }
        if (dctx != nullptr) {
            ZSTD_freeDCtx(dctx);
        }
        return false;
    }
    _cctx = cctx;
    _dctx = dctx;

    if (!dict_path.empty()) {
        std::ifstream f(dict_path, std::ios::binary | std::ios::ate);
        if (f) {
            std::streamsize n = f.tellg();
            if (n > 0) {
                _dict.resize((size_t)n);
                f.seekg(0);
                if (f.read((char *)_dict.data(), n)) {
                    _cdict = ZSTD_createCDict(_dict.data(), _dict.size(), _level);
                    _ddict = ZSTD_createDDict(_dict.data(), _dict.size());
                }
            }
        }
        if (_cdict == nullptr || _ddict == nullptr) {
            log_warning("ZSTD: could not load dictionary '%s'; compressing without it",
                        dict_path.c_str());
        } else {
            log_info("ZSTD: loaded %zu-byte dictionary '%s' (level %d)",
                     _dict.size(),
                     dict_path.c_str(),
                     _level);
        }
    } else {
        log_info("ZSTD: compression enabled without dictionary (level %d)", _level);
    }

    _cbuf.resize(ZSTD_compressBound(kMaxPacket));
    _dbuf.resize(kMaxPacket);
    return true;
}

const uint8_t *ZstdCodec::compress(const uint8_t *in, size_t in_len, size_t *out_len)
{
    uint32_t id;
    if (first_msg_id(in, in_len, &id) && is_incompressible_msg_id(id)) {
        return nullptr; // time-critical message -> send plain
    }
    if (in_len == 0 || in_len > kMaxPacket) {
        return nullptr; // nothing to do / too large for scratch -> send plain
    }

    size_t c;
    if (_cdict != nullptr) {
        c = ZSTD_compress_usingCDict((ZSTD_CCtx *)_cctx,
                                     _cbuf.data(),
                                     _cbuf.size(),
                                     in,
                                     in_len,
                                     (const ZSTD_CDict *)_cdict);
    } else {
        c = ZSTD_compressCCtx((ZSTD_CCtx *)_cctx, _cbuf.data(), _cbuf.size(), in, in_len, _level);
    }
    if (ZSTD_isError(c) || c == 0 || c >= in_len) {
        return nullptr; // error or did not shrink -> send plain
    }
    *out_len = c;
    return _cbuf.data();
}

ssize_t ZstdCodec::inflate(uint8_t *buf, size_t in_len, size_t cap)
{
    if (!looks_compressed(buf, in_len)) {
        return (ssize_t)in_len; // plain MAVLink -> pass through untouched
    }

    size_t d;
    if (_ddict != nullptr) {
        d = ZSTD_decompress_usingDDict((ZSTD_DCtx *)_dctx,
                                       _dbuf.data(),
                                       _dbuf.size(),
                                       buf,
                                       in_len,
                                       (const ZSTD_DDict *)_ddict);
    } else {
        d = ZSTD_decompressDCtx((ZSTD_DCtx *)_dctx, _dbuf.data(), _dbuf.size(), buf, in_len);
    }
    if (ZSTD_isError(d)) {
        log_debug("ZSTD: decompress error: %s", ZSTD_getErrorName(d));
        return -1;
    }
    if (d > cap) {
        log_warning("ZSTD: decompressed %zu bytes > rx buffer %zu; dropping datagram", d, cap);
        return -1;
    }
    memcpy(buf, _dbuf.data(), d);
    return (ssize_t)d;
}

#else // !HAVE_ZSTD -- stub: the feature was not built in.

ZstdCodec::~ZstdCodec() = default;

bool ZstdCodec::init(const std::string & /*dict_path*/, int /*level*/)
{
    log_error("ZstdCompression is set, but mavlink-router was built without ZSTD "
              "support (libzstd was not found at build time)");
    return false;
}

const uint8_t *ZstdCodec::compress(const uint8_t * /*in*/, size_t /*in_len*/, size_t * /*out_len*/)
{
    return nullptr;
}

ssize_t ZstdCodec::inflate(uint8_t * /*buf*/, size_t in_len, size_t /*cap*/)
{
    return (ssize_t)in_len;
}

#endif // HAVE_ZSTD
