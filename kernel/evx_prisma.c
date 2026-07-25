// SPDX-License-Identifier: GPL-2.0
/*
 * SECOND-NEES PrismaAuthD kernel factor provider.
 *
 * Userspace supplies the public nonce stored in the encrypted runtime blob.
 * The matching Evonix kernel returns:
 *
 *     HMAC-SHA256(kernel_secret, nonce)
 *
 * The response is used as the fourth PrismaAuthD decryption factor.
 */

#include <crypto/hash.h>

#include <linux/cred.h>
#include <linux/crypto.h>
#include <linux/errno.h>
#include <linux/evx_prisma.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#include "evx_prisma_secret.h"

long evx_prisma_prctl(unsigned long nonce_address,
                      unsigned long response_address,
                      unsigned long nonce_size,
                      unsigned long protocol_version)
{
    struct crypto_shash *transform = NULL;
    struct shash_desc *description = NULL;
    u8 nonce[EVX_PRISMA_NONCE_SIZE];
    u8 response[EVX_PRISMA_RESPONSE_SIZE];
    size_t description_size;
    long result = 0;

    /*
     * PrismaAuthD is launched by Android init as root. This prevents normal
     * applications from querying the kernel factor directly.
     */
    if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
        return -EPERM;

    if (!nonce_address || !response_address)
        return -EINVAL;

    if (nonce_size != EVX_PRISMA_NONCE_SIZE)
        return -EINVAL;

    if (protocol_version != EVX_PRISMA_PROTOCOL_VERSION)
        return -EPROTONOSUPPORT;

    if (copy_from_user(nonce,
                       (const void __user *)nonce_address,
                       sizeof(nonce))) {
        return -EFAULT;
    }

    transform = crypto_alloc_shash("hmac(sha256)", 0, 0);

    if (IS_ERR(transform)) {
        result = PTR_ERR(transform);
        transform = NULL;
        goto cleanup;
    }

    result = crypto_shash_setkey(transform,
                                 evx_prisma_kernel_secret,
                                 sizeof(evx_prisma_kernel_secret));

    if (result)
        goto cleanup;

    description_size = sizeof(*description) +
                       crypto_shash_descsize(transform);

    description = kzalloc(description_size, GFP_KERNEL);

    if (!description) {
        result = -ENOMEM;
        goto cleanup;
    }

    description->tfm = transform;

    result = crypto_shash_digest(description,
                                 nonce,
                                 sizeof(nonce),
                                 response);

    if (result)
        goto cleanup;

    if (copy_to_user((void __user *)response_address,
                     response,
                     sizeof(response))) {
        result = -EFAULT;
        goto cleanup;
    }

cleanup:
    memzero_explicit(nonce, sizeof(nonce));
    memzero_explicit(response, sizeof(response));

    kfree(description);

    if (transform)
        crypto_free_shash(transform);

    return result;
}
