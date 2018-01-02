/*!
 * @file
 * @brief ubirch protocol implementation based on msgpack
 *
 * The basic ubirch protocol implementation based on msgpack.
 * A ubirch protocol message consists of a header, payload and
 * a signature. The signature is calculated from the streaming
 * hash (SHA256) of the msgpack data in front of the signature,
 * excluding the msgpack type marker for the signature.
 *
 * The generation of messages is similar to msgpack:
 *
 * ```
 * // creata a standard msgpack stream buffer
 * msgpack_sbuffer *sbuf = msgpack_sbuffer_new();
 * // create a ubirch protocol context from the buffer, its writer
 * // and provide the signature function as well as the UUID
 * ubirch_protocol *proto = ubirch_protocol_new(sbuf, msgpack_sbuffer_write, ed25519_sign,
 *                                              (const unsigned char *) UUID);
 * // create a msgpack packer from the ubirch protocol
 * msgpack_packer *pk = msgpack_packer_new(proto, ubirch_protocol_write);
 *
 * // pack a message by starting with the header
 * ubirch_protocol_start(proto, pk);
 * // add payload (must be a single element, use map/array for multiple data points)
 * msgpack_pack_int(pk, 99);
 * // finish the message (calculates signature)
 * ubirch_protocol_finish(proto, pk);
 * ```
 * 
 * The protocol context takes care of hashing and sending the data to
 * the stream buffer. Instead of a stream buffer, the data may be
 * written directly to the network using a custom write function instead of
 * `msgpack_sbuffer_write`.
 *
 * @author Matthias L. Jugel
 * @date   2018-01-01
 *
 * @copyright &copy; 2018 ubirch GmbH (https://ubirch.com)
 *
 * ```
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ```
 */

#include <msgpack.h>

#ifdef MBEDTLS_CONFIG_FILE
#include <mbedtls/sha256.h>
#include <mbed-os/features/mbedtls/inc/mbedtls/sha256.h>

#else
#include "sha256.h"
#endif
#define UBIRCH_PROTOCOL_VERSION     0x0401  //!< current ubirch protocol version
#define UBIRCH_PROTOCOL_SIGN_SIZE   64      //!< our signatures has 64 bytes

/**
 * The signature function type necessary to sign the message for the ubirch protocol.
 * This function is called from #ubirch_protocol_finish
 *
 * @param buf the data to sign
 * @param size_t len the length of the data buffer
 * @param signature signature output (64 bytes)
 */
typedef int (*ubirch_protocol_sign)(const char *buf, size_t len, unsigned char signature[UBIRCH_PROTOCOL_SIGN_SIZE]);

/**
 * ubirch protocol context, which holds the underlying packer, the uuid and current and previous signature
 * as well as the current hash.
 */
typedef struct ubirch_protocol {
    msgpack_packer packer;                              //!< the underlying target packer
    ubirch_protocol_sign sign;                          //!< the message signing function
    unsigned char uuid[16];                             //!< the uuid of the sender (used to retrieve the keys)
    unsigned char signature[UBIRCH_PROTOCOL_SIGN_SIZE]; //!< the current or previous signature of a message
    mbedtls_sha256_context hash;                        //!< the streaming hash of the data to sign
} ubirch_protocol;

/**
 * Initialize a new ubirch protocol context.
 *
 * @param proto the ubirch protocol context
 * @param data the data object buffer associated with the context
 * @param callback the writer callback for writing data to network or buffer
 * @param sign a callback used for signing a message
 * @param uuid the uuid associated with the data
 */
static void ubirch_protocol_init(ubirch_protocol *proto, void *data, msgpack_packer_write callback,
                                 ubirch_protocol_sign sign, const unsigned char uuid[16]);

/**
 * Create a new ubirch protocol context.
 *
 * @param data the data object buffer associated with the context
 * @param callback the writer callback for writing data to network or buffer
 * @param sign a callback used for signing a message
 * @param uuid the uuid associated with the data
 * @return a new initialized context
 */
static ubirch_protocol *ubirch_protocol_new(void *data, msgpack_packer_write callback,
                                            ubirch_protocol_sign sign, const unsigned char uuid[16]);

/**
 * Free memory for a ubirch protocol context.
 * @param proto the protocol context
 */
static void ubirch_protocol_free(ubirch_protocol *proto);

/**
 * Start a new message. Clears out previous data and re-initialized the signature
 * handler. Also writes the header data.
 * @param proto the ubirch protocol context
 * @param pk the msgpack packer used for serializing data
 */
static void ubirch_protocol_start(ubirch_protocol *proto, msgpack_packer *pk);

/**
 * Finish a message. Calculates the signature and attaches it to the message.
 * @param proto the ubirch protocol context
 * @param pk the msgpack packer used for serializing data
 * @return 0 if successful, 1 if either packer or protocol are NULL, 2 if used before ubirch_protocol_start
 */
static int ubirch_protocol_finish(ubirch_protocol *proto, msgpack_packer *pk);

/**
 * The ubirch protocol msgpack writer. This writer takes care of updating the hash
 * and writing original data to the underlying write callback.
 * @param data the ubirch protocol context
 * @param buf the data to writer and hash
 * @param len the length of the data
 * @return 0 if successful
 */
static inline int ubirch_protocol_write(void *data, const char *buf, size_t len) {
    ubirch_protocol *proto = (ubirch_protocol *) data;
    mbedtls_sha256_update(&proto->hash, (const unsigned char *) buf, len);
    return proto->packer.callback(proto->packer.data, buf, len);
}

inline void ubirch_protocol_init(ubirch_protocol *proto, void *data, msgpack_packer_write callback,
                                 ubirch_protocol_sign sign, const unsigned char uuid[16]) {
    proto->packer.data = data;
    proto->packer.callback = callback;
    proto->sign = sign;
    proto->hash.is224 = -1;
    memcpy(proto->uuid, uuid, 16);
}

inline ubirch_protocol *ubirch_protocol_new(void *data, msgpack_packer_write callback,
                                            ubirch_protocol_sign sign, const unsigned char uuid[16]) {
    ubirch_protocol *proto = (ubirch_protocol *) calloc(1, sizeof(ubirch_protocol));
    if (!proto) { return NULL; }
    ubirch_protocol_init(proto, data, callback, sign, uuid);

    return proto;
}

inline void ubirch_protocol_free(ubirch_protocol *proto) {
    free(proto);
}

inline void ubirch_protocol_start(ubirch_protocol *proto, msgpack_packer *pk) {
    if (proto == NULL || pk == NULL) return;

    mbedtls_sha256_init(&proto->hash);
    mbedtls_sha256_starts(&proto->hash, 0);

    // the message consists of 3 header elements, the payload and (not included) the signature
    msgpack_pack_array(pk, 5);

    // 1 - protocol version
    msgpack_pack_fix_uint16(pk, UBIRCH_PROTOCOL_VERSION);

    // 2 - device ID
    msgpack_pack_raw(pk, 16);
    msgpack_pack_raw_body(pk, proto->uuid, sizeof(proto->uuid));

    // 3 the last signature
    msgpack_pack_raw(pk, sizeof(proto->signature));
    msgpack_pack_raw_body(pk, proto->signature, sizeof(proto->signature));
}

inline int ubirch_protocol_finish(ubirch_protocol *proto, msgpack_packer *pk) {
    if (proto == NULL || pk == NULL) return 1;
    if(proto->hash.is224 == -1) return 2;

    unsigned char sha256sum[32];
    mbedtls_sha256_finish(&proto->hash, sha256sum);
    if(proto->sign((const char *) sha256sum, sizeof(sha256sum), proto->signature)) {
        return 3;
    }

    // 5 add signature hash
    msgpack_pack_raw(pk, UBIRCH_PROTOCOL_SIGN_SIZE);
    msgpack_pack_raw_body(pk, proto->signature, UBIRCH_PROTOCOL_SIGN_SIZE);

    return 0;
}

