/* Copyright 2013 Justin Erenkrantz and Greg Stein
 *
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
 */

#ifdef SERF_HAVE_SECURETRANSPORT

#include "serf.h"
#include "serf_private.h"
#include "serf_bucket_util.h"
#include "bucket_private.h"

#include <apr_strings.h>
#include <apr_base64.h>

#include <Security/SecureTransport.h>
#include <Security/SecPolicy.h>
#include <Security/SecImportExport.h>
#include <objc/runtime.h>
#include <objc/message.h>

#define SECURE_TRANSPORT_READ_BUFSIZE 8000

static OSStatus
sectrans_read_cb(SSLConnectionRef connection, void *data, size_t *dataLength);
static OSStatus
sectrans_write_cb(SSLConnectionRef connection, const void *data, size_t *dataLength);

typedef struct sectrans_ssl_stream_t {
    /* For an encrypt stream: data encrypted & not yet written to the network.
       For a decrypt stream: data decrypted & not yet read by the application.*/
    serf_bucket_t *pending;

    /* For an encrypt stream: the outgoing data provided by the application.
       For a decrypt stream: encrypted data read from the network. */
    serf_bucket_t *stream;
} sectrans_ssl_stream_t;


/* States for the different stages in the lifecyle of an SSL session. */
typedef enum {
    SERF_SECTRANS_INIT,       /* no SSL handshake yet */
    SERF_SECTRANS_HANDSHAKE,  /* SSL handshake in progress */
    SERF_SECTRANS_CONNECTED,  /* SSL handshake successfully finished */
    SERF_SECTRANS_CLOSING,    /* SSL session closing */
} sectrans_session_state_t;

typedef struct sectrans_context_t {
    /* How many open buckets refer to this context. */
    int refcount;

    serf_bucket_alloc_t *allocator;

    SSLContextRef st_ctxr;

    /* stream of (to be) encrypted data, outgoing to the network. */
    sectrans_ssl_stream_t encrypt;

    /* stream of (to be) decrypted data, read from the network. */
    sectrans_ssl_stream_t decrypt;

    sectrans_session_state_t state;

    /* name of the peer, used with TLS's Server Name Indication extension. */
    char *hostname;

    /* allowed modes for certification validation, see enum
       serf_ssl_cert_validation_mode_t for more info. */
    int modes;

    /* Server cert callbacks */
    serf_ssl_need_server_cert_t server_cert_callback;
    serf_ssl_server_cert_chain_cb_t server_cert_chain_callback;
    void *server_cert_userdata;
    
} sectrans_context_t;

static apr_status_t
translate_sectrans_status(OSStatus status)
{
    switch (status)
    {
        case noErr:
            return APR_SUCCESS;
        case errSSLWouldBlock:
            return APR_EAGAIN;
        default:
            serf__log(SSL_VERBOSE, __FILE__,
                      "Unknown Secure Transport error %d\n", status);
            return APR_EGENERAL;
    }
}

/* Callback function for the encrypt.pending and decrypt.pending stream-type
   aggregate buckets.
 */
apr_status_t pending_stream_eof(void *baton,
                                serf_bucket_t *pending)
{
    /* Both pending streams have to stay open so that the Secure Transport
       library can keep appending data buckets. */
    return APR_EAGAIN;
}

static sectrans_context_t *
sectrans_init_context(serf_bucket_alloc_t *allocator)
{
    sectrans_context_t *ssl_ctx;

    ssl_ctx = serf_bucket_mem_alloc(allocator, sizeof(*ssl_ctx));
    ssl_ctx->refcount = 0;

    /* Default mode: validate certificates against KeyChain without GUI.
       If a certificate needs to be confirmed by the user, error out. */
    ssl_ctx->modes = serf_ssl_val_mode_serf_managed_no_gui;

    /* Set up the stream objects. */
    ssl_ctx->encrypt.pending = serf__bucket_stream_create(allocator,
                                                          pending_stream_eof,
                                                          NULL);
    ssl_ctx->decrypt.pending = serf__bucket_stream_create(allocator,
                                                          pending_stream_eof,
                                                          NULL);

    /* Set up a Secure Transport session. */
    ssl_ctx->state = SERF_SECTRANS_INIT;

    if (SSLNewContext(FALSE, &ssl_ctx->st_ctxr))
        return NULL;

    if (SSLSetIOFuncs(ssl_ctx->st_ctxr, sectrans_read_cb, sectrans_write_cb))
        return NULL;

    /* Ensure the sectrans_context will be passed to the read and write callback
       functions. */
    if (SSLSetConnection(ssl_ctx->st_ctxr, ssl_ctx))
        return NULL;

    /* We do our own validation of server certificates.
       Note that Secure Transport will not do any validation with this option
       enabled, it's all or nothing. */
    if (SSLSetSessionOption(ssl_ctx->st_ctxr,
                            kSSLSessionOptionBreakOnServerAuth,
                            true))
        return NULL;
    if (SSLSetEnableCertVerify(ssl_ctx->st_ctxr, false))
        return NULL;

    return ssl_ctx;
}

static apr_status_t
sectrans_free_context(sectrans_context_t * ctx, serf_bucket_alloc_t *allocator)
{
    OSStatus status = SSLDisposeContext (ctx->st_ctxr);

    serf_bucket_mem_free(allocator, ctx);

    if (status)
        return APR_EGENERAL;

    return APR_SUCCESS;
}

/**
 * Note for both read and write callback functions, from SecureTransport.h:
 * "Data's memory is allocated by caller; on entry to these two functions
 *  the *length argument indicates both the size of the available data and the
 *  requested byte count. Number of bytes actually transferred is returned in
 *  *length."
 **/

/** Secure Transport callback function.
    Reads encrypted data from the network. **/
static OSStatus
sectrans_read_cb(SSLConnectionRef connection,
                 void *data,
                 size_t *dataLength)
{
    const sectrans_context_t *ssl_ctx = connection;
    apr_status_t status = 0;
    const char *buf;
    char *outbuf = data;
    size_t requested = *dataLength, buflen = 0;

    serf__log(SSL_VERBOSE, __FILE__, "sectrans_read_cb called for "
              "%d bytes.\n", requested);

    *dataLength = 0;
    while (!status && requested) {
        status = serf_bucket_read(ssl_ctx->decrypt.stream, requested,
                                  &buf, &buflen);

        if (SERF_BUCKET_READ_ERROR(status)) {
            serf__log(SSL_VERBOSE, __FILE__, "Returned status %d.\n", status);
            return -1;
        }

        if (buflen) {
            serf__log(SSL_VERBOSE, __FILE__, "Read %d bytes with status %d.\n",
                      buflen, status);

            /* Copy the data in the buffer provided by the caller. */
            memcpy(outbuf, buf, buflen);
            outbuf += buflen;
            requested -= buflen;
            (*dataLength) += buflen;
        }
    }

    if (APR_STATUS_IS_EAGAIN(status))
        return errSSLWouldBlock;

    if (!status)
        return noErr;

    /* TODO: map apr status to Mac OS X error codes(??) */
    return -1;
}

/** Secure Transport callback function.
    Writes encrypted data to the network. **/
static OSStatus
sectrans_write_cb(SSLConnectionRef connection,
                  const void *data,
                  size_t *dataLength)
{
    serf_bucket_t *tmp;
    const sectrans_context_t *ctx = connection;

    serf__log(SSL_VERBOSE, __FILE__, "sectrans_write_cb called for "
              "%d bytes.\n", *dataLength);

    tmp = serf_bucket_simple_copy_create(data, *dataLength,
                                         ctx->encrypt.pending->allocator);

    serf_bucket_aggregate_append(ctx->encrypt.pending, tmp);

    return noErr;
}

/* Show a SFCertificateTrustPanel. This is the Mac OS X default dialog to
   ask the user to confirm or deny the use of the certificate. This panel
   also gives the option to store the user's decision for this certificate
   permantly in the Keychain (requires password).
 */

/* TODO: serf or application? If serf, let appl. customize labels. If 
   application, how to get SecTrustRef object back to app? */
static apr_status_t
ask_approval_gui(sectrans_context_t *ssl_ctx, SecTrustRef trust)
{
    const CFStringRef OkButtonLbl = CFSTR("Accept");
    const CFStringRef CancelButtonLbl = CFSTR("Cancel");
    const CFStringRef Message = CFSTR("The server certificate requires validation.");

    /* Creates an NSApplication object (enables GUI for cocoa apps) if one
       doesn't exist already. */
    void *nsapp_cls = objc_getClass("NSApplication");
    (void) objc_msgSend(nsapp_cls,sel_registerName("sharedApplication"));

    void *stp_cls = objc_getClass("SFCertificateTrustPanel");
    void *stp = objc_msgSend(stp_cls, sel_registerName("alloc"));
    stp = objc_msgSend(stp, sel_registerName("init"));

    /* TODO: find a way to get the panel in front of all other windows. */

    /* Don't use these methods as is, they create a small application window
       and have no effect on the z-order of the modal dialog. */
//    objc_msgSend(obj, sel_getUid("orderFrontRegardless"));
//    objc_msgSend (obj, sel_getUid ("makeKeyAndOrderFront:"), app);

//    objc_msgSend (nsapp, sel_getUid ("activateIgnoringOtherApps:"), 1);
//    objc_msgSend (stp, sel_getUid ("makeKeyWindow"));

    /* Setting name of the cancel button also makes it visible on the panel. */
    objc_msgSend(stp, sel_getUid("setDefaultButtonTitle:"), OkButtonLbl);
    objc_msgSend(stp, sel_getUid("setAlternateButtonTitle:"), CancelButtonLbl);
    
    long result = (long)objc_msgSend(stp,
                                     sel_getUid("runModalForTrust:message:"),
                                     trust, Message);
    serf__log(SSL_VERBOSE, __FILE__, "User clicked %s button.\n",
              result ? "Accept" : "Cancel");

    if (result) /* NSOKButton = 1 */
        return APR_SUCCESS;
    else        /* NSCancelButton = 0 */
        return SERF_ERROR_SSL_USER_DENIED_CERT;
}

/* Validate a server certificate. Call back to the application if needed.
   Returns APR_SUCCESS if the server certificate is accepted.
   Otherwise returns an error.
 */
static int
validate_server_certificate(sectrans_context_t *ssl_ctx)
{
    OSStatus sectrans_status;
    CFArrayRef certs;
    SecTrustRef trust;
    SecTrustResultType result;
    int failures = 0;
    size_t depth_of_error;
    apr_status_t status;

    serf__log(SSL_VERBOSE, __FILE__, "validate_server_certificate called.\n");

    /* Get the server certificate chain. */
    sectrans_status = SSLCopyPeerCertificates(ssl_ctx->st_ctxr, &certs);
    if (sectrans_status != noErr)
        return translate_sectrans_status(sectrans_status);
    /* TODO: 0, oh really? How can we know where the error occurred? */
    depth_of_error = 0;

    sectrans_status = SSLCopyPeerTrust(ssl_ctx->st_ctxr, &trust);
    if (sectrans_status != noErr) {
        status = translate_sectrans_status(sectrans_status);
        goto cleanup;
    }

    /* TODO: SecTrustEvaluateAsync */
    sectrans_status = SecTrustEvaluate(trust, &result);
    if (sectrans_status != noErr) {
        status = translate_sectrans_status(sectrans_status);
        goto cleanup;
    }

    /* Based on the contents of the user's Keychain, Secure Transport will make
       a first validation of this certificate chain.
       The status set here is temporary, as it can be overridden by the
       application. */
    switch (result)
    {
        case kSecTrustResultUnspecified:
        case kSecTrustResultProceed:
            serf__log(SSL_VERBOSE, __FILE__,
                      "kSecTrustResultProceed/Unspecified.\n");
            failures = SERF_SSL_CERT_ALL_OK;
            status = APR_SUCCESS;
            break;
        case kSecTrustResultConfirm:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultConfirm.\n");
            failures = SERF_SSL_CERT_CONFIRM_NEEDED |
                       SERF_SSL_CERT_RECOVERABLE;
            break;
        case kSecTrustResultRecoverableTrustFailure:
            serf__log(SSL_VERBOSE, __FILE__,
                      "kSecTrustResultRecoverableTrustFailure.\n");
            failures = SERF_SSL_CERT_UNKNOWN_FAILURE |
                       SERF_SSL_CERT_RECOVERABLE;
            break;

        /* Fatal errors */
        case kSecTrustResultInvalid:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultInvalid.\n");
            failures = SERF_SSL_CERT_FATAL;
            status = SERF_ERROR_SSL_CERT_FAILED;
            break;
        case kSecTrustResultDeny:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultDeny.\n");
            failures = SERF_SSL_CERT_FATAL;
            status = SERF_ERROR_SSL_KEYCHAIN_DENIED_CERT;
            break;
        case kSecTrustResultFatalTrustFailure:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultFatalTrustFailure.\n");
            failures = SERF_SSL_CERT_FATAL;
            status = SERF_ERROR_SSL_CERT_FAILED;
            break;
        case kSecTrustResultOtherError:
            serf__log(SSL_VERBOSE, __FILE__, "kSecTrustResultOtherError.\n");
            failures = SERF_SSL_CERT_FATAL;
            status = SERF_ERROR_SSL_CERT_FAILED;
            break;
        default:
            serf__log(SSL_VERBOSE, __FILE__, "unknown.\n");
            failures = SERF_SSL_CERT_FATAL;
            status = SERF_ERROR_SSL_CERT_FAILED;
            break;
    }

    /* Recoverable errors? Ask the user for confirmation. */
    if (failures & SERF_SSL_CERT_CONFIRM_NEEDED ||
        failures & SERF_SSL_CERT_RECOVERABLE)
    {
        if (ssl_ctx->modes & serf_ssl_val_mode_serf_managed_with_gui)
        {
            status = ask_approval_gui(ssl_ctx, trust);
            /* TODO: remember this approval for 'some time' ! */
            goto cleanup;
        } else
        {
            status = SERF_ERROR_SSL_CANT_CONFIRM_CERT;
        }
    }

    /* If serf can take the decision, don't call back to the application. */
    if (failures & SERF_SSL_CERT_ALL_OK ||
        failures & SERF_SSL_CERT_FATAL)
    {
        if (ssl_ctx->modes & serf_ssl_val_mode_serf_managed_with_gui ||
            ssl_ctx->modes & serf_ssl_val_mode_serf_managed_no_gui)
        {
            /* The application allowed us to take the decision. */
            goto cleanup;
        }
    }

    /* Ask the application to validate the certificate. */
    if ((ssl_ctx->modes & serf_ssl_val_mode_application_managed) &&
        (ssl_ctx->server_cert_callback && failures))
    {
        serf_ssl_certificate_t *cert;
        sectrans_certificate_t *sectrans_cert;

        sectrans_cert = serf_bucket_mem_alloc(ssl_ctx->allocator,
                                          sizeof(sectrans_certificate_t));
        sectrans_cert->content = NULL;
        sectrans_cert->certref = (SecCertificateRef)CFArrayGetValueAtIndex(certs, 0);

        cert = serf__create_certificate(ssl_ctx->allocator,
                                        &serf_ssl_bucket_type_securetransport,
                                        sectrans_cert,
                                        depth_of_error);

        /* Callback for further verification. */
        status = ssl_ctx->server_cert_callback(ssl_ctx->server_cert_userdata,
                                               failures, cert);

        serf_bucket_mem_free(ssl_ctx->allocator, cert);
        goto cleanup;
    } else
    {
        status = SERF_ERROR_SSL_CERT_FAILED;
    }
cleanup:
    CFRelease(certs);
    CFRelease(trust);

    return status;
}

/* Run the SSL handshake. */
static apr_status_t do_handshake(sectrans_context_t *ssl_ctx)
{
    OSStatus sectrans_status;
    apr_status_t status;

    serf__log(SSL_VERBOSE, __FILE__, "do_handshake called.\n");

    sectrans_status = SSLHandshake(ssl_ctx->st_ctxr);
    if (sectrans_status)
        serf__log(SSL_VERBOSE, __FILE__, "do_handshake returned err %d.\n",
                  sectrans_status);

    switch(sectrans_status) {
        case noErr:
            status = APR_SUCCESS;
            break;
        case errSSLServerAuthCompleted:
            /* Server's cert validation was disabled, so we can to do this
               here. */
            status = validate_server_certificate(ssl_ctx);
            if (!status)
                return APR_EAGAIN;
            break;
        case errSSLClientCertRequested:
            return APR_ENOTIMPL;
        default:
            status = translate_sectrans_status(sectrans_status);
            break;
    }

    return status;
}


/**** SSL_BUCKET API ****/
/************************/
static void *
decrypt_create(serf_bucket_t *bucket,
               serf_bucket_t *stream,
               void *impl_ctx,
               serf_bucket_alloc_t *allocator)
{
    sectrans_context_t *ssl_ctx;
    bucket->type = &serf_bucket_type_sectrans_decrypt;
    bucket->allocator = allocator;

    if (impl_ctx)
        bucket->data = impl_ctx;
    else
        bucket->data = sectrans_init_context(allocator);

    ssl_ctx = bucket->data;
    ssl_ctx->refcount++;
    ssl_ctx->decrypt.stream = stream;
    ssl_ctx->allocator = allocator;

    return bucket->data;
}

static void *
encrypt_create(serf_bucket_t *bucket,
               serf_bucket_t *stream,
               void *impl_ctx,
               serf_bucket_alloc_t *allocator)
{
    sectrans_context_t *ssl_ctx;
    bucket->type = &serf_bucket_type_sectrans_encrypt;
    bucket->allocator = allocator;

    if (impl_ctx)
        bucket->data = impl_ctx;
    else
        bucket->data = sectrans_init_context(allocator);

    ssl_ctx = bucket->data;
    ssl_ctx->refcount++;
    ssl_ctx->encrypt.stream = stream;
    ssl_ctx->allocator = allocator;

    return bucket->data;
}

static void *
decrypt_context_get(serf_bucket_t *bucket)
{
    return NULL;
}

static void *
encrypt_context_get(serf_bucket_t *bucket)
{
    return NULL;
}


static void
client_cert_provider_set(void *impl_ctx,
                         serf_ssl_need_client_cert_t callback,
                         void *data,
                         void *cache_pool)
{
    return;
}


static void
client_cert_password_set(void *impl_ctx,
                         serf_ssl_need_cert_password_t callback,
                         void *data,
                         void *cache_pool)
{
    sectrans_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->modes |= serf_ssl_val_mode_application_managed;
    
    return;
}


void server_cert_callback_set(void *impl_ctx,
                              serf_ssl_need_server_cert_t callback,
                              void *data)
{
    sectrans_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->modes |= serf_ssl_val_mode_application_managed;

    ssl_ctx->server_cert_callback = callback;
    ssl_ctx->server_cert_userdata = data;
}

void server_cert_chain_callback_set(void *impl_ctx,
                                    serf_ssl_need_server_cert_t cert_callback,
                                    serf_ssl_server_cert_chain_cb_t cert_chain_callback,
                                    void *data)
{
    sectrans_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->modes |= serf_ssl_val_mode_application_managed;

    ssl_ctx->server_cert_callback = cert_callback;
    ssl_ctx->server_cert_chain_callback = cert_chain_callback;
    ssl_ctx->server_cert_userdata = data;
}

static apr_status_t
set_hostname(void *impl_ctx, const char * hostname)
{
    sectrans_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->hostname = serf_bstrdup(ssl_ctx->allocator, hostname);
    OSStatus status = SSLSetPeerDomainName(ssl_ctx->st_ctxr,
                                           ssl_ctx->hostname,
                                           strlen(hostname));
    return status;
}

static apr_status_t
use_default_certificates(void *impl_ctx)
{
    /* Secure transport uses default certificates automatically.
       TODO: verify if this true. */
    return APR_SUCCESS;
}

/* Find the file extension, if any.
   Copied the original code & comments from the Apache Subversion project. */
const char * splitext(const char *path)
{
    const char *last_dot, *last_slash;

    /* Do we even have a period in this thing?  And if so, is there
       anything after it?  We look for the "rightmost" period in the
       string. */
    last_dot = strrchr(path, '.');
    if (last_dot && (last_dot + 1 != '\0')) {
        /* If we have a period, we need to make sure it occurs in the
           final path component -- that there's no path separator
           between the last period and the end of the PATH -- otherwise,
           it doesn't count.  Also, we want to make sure that our period
           isn't the first character of the last component. */
        last_slash = strrchr(path, '/');
        if ((last_slash && (last_dot > (last_slash + 1)))
            || ((! last_slash) && (last_dot > path))) {
                return last_dot + 1;
        }
    }

    return "";
}

/* Copies the unicode string from a CFStringRef to a new buffer allocated
   from pool. */
static const char *
CFStringToChar(CFStringRef str, apr_pool_t *pool)
{
    const char *ptr = CFStringGetCStringPtr(str, kCFStringEncodingMacRoman);

    if (ptr == NULL) {
        const int strlen = CFStringGetLength(str) * 2;
        char *buf = apr_pcalloc(pool, strlen);
        if (CFStringGetCString(str, buf, strlen, kCFStringEncodingMacRoman))
            return buf;
    } else {
        return apr_pstrdup(pool, ptr);
    }

    return NULL;
}

static apr_status_t
load_CA_cert_from_file(serf_ssl_certificate_t **cert,
                       const char *file_path,
                       apr_pool_t *pool)
{
    apr_file_t *fp;
    apr_status_t status;

    status = apr_file_open(&fp, file_path,
                           APR_FOPEN_READ | APR_FOPEN_BINARY,
                           APR_FPROT_OS_DEFAULT, pool);
    if (!status) {
        const char *ext;
        OSStatus osstatus;
        apr_finfo_t file_info;
        char *buf;
        apr_size_t len;
        CFDataRef databuf;
        CFArrayRef items;
        CFStringRef extref;
        SecExternalItemType itemType;

        /* Read the file in memory */
        apr_file_info_get(&file_info, APR_FINFO_SIZE, fp);
        buf = apr_palloc(pool, file_info.size);

        status = apr_file_read_full(fp, buf, file_info.size, &len);
        if (status)
            return status;

        ext = splitext(file_path);
        extref = CFStringCreateWithBytesNoCopy(kCFAllocatorDefault,
                                               (unsigned char *)ext,
                                               strlen(ext),
                                               kCFStringEncodingMacRoman,
                                               false,
                                               kCFAllocatorNull);

        itemType = kSecItemTypeUnknown;
        databuf = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                                              (unsigned char *)buf,
                                              file_info.size,
                                              kCFAllocatorNull);

        osstatus = SecItemImport(databuf, extref,
                                 kSecFormatUnknown,
                                 &itemType,
                                 0,    /* SecItemImportExportFlags */
                                 NULL, /* SecItemImportExportKeyParameters */
                                 NULL, /* SecKeychainRef */
                                 &items);

        if (osstatus != errSecSuccess) {
#if SSL_VERBOSE
            CFStringRef errref = SecCopyErrorMessageString(osstatus, NULL);
            const char *errstr = CFStringToChar(errref, pool);

            serf__log(SSL_VERBOSE, __FILE__, "Error loading certificate: %s.\n",
                      errstr);
#endif
            return SERF_ERROR_SSL_CERT_FAILED;
        }

        if (itemType == kSecItemTypeCertificate && CFArrayGetCount(items) > 0) {
            SecCertificateRef ssl_cert = (SecCertificateRef)CFArrayGetValueAtIndex(items, 0);

            if (ssl_cert) {
                sectrans_certificate_t *sectrans_cert;
                serf_bucket_alloc_t *allocator =
                    serf_bucket_allocator_create(pool, NULL, NULL);

                sectrans_cert = serf_bucket_mem_alloc(allocator,
                                    sizeof(sectrans_certificate_t));
                sectrans_cert->content = NULL;
                sectrans_cert->certref = ssl_cert;

                *cert = serf__create_certificate(allocator,
                                                 &serf_ssl_bucket_type_securetransport,
                                                 sectrans_cert,
                                                 0);

                return APR_SUCCESS;
            }
        }
    }

    return SERF_ERROR_SSL_CERT_FAILED;
}


static apr_status_t trust_cert(void *impl_ctx,
                               serf_ssl_certificate_t *cert)
{
    sectrans_context_t *ssl_ctx = impl_ctx;
    sectrans_certificate_t *sectrans_cert = cert->impl_cert;
    OSStatus sectrans_status;

    SecCertificateRef certs[1] = { sectrans_cert->certref };
    CFArrayRef certarray = CFArrayCreate(kCFAllocatorDefault,
                                         (void *)certs,
                                         1,
                                         NULL);

    /* Add the certificate to the current list. */
    sectrans_status = SSLSetTrustedRoots(ssl_ctx->st_ctxr, certarray, false);
    CFRelease(certarray);
    return translate_sectrans_status(sectrans_status);
}

apr_hash_t *cert_certificate(const serf_ssl_certificate_t *cert,
                             apr_pool_t *pool)
{
    apr_hash_t *tgt;
    const char *date_str, *sha1;

    sectrans_certificate_t *sectrans_cert = cert->impl_cert;

    if (!sectrans_cert->content) {
        apr_status_t status;
        status = serf__sectrans_read_X509_DER_certificate(&sectrans_cert->content,
                                                          sectrans_cert,
                                                          pool);
        if (status)
            return NULL;
    }

    tgt = apr_hash_make(pool);

    date_str = apr_hash_get(sectrans_cert->content, "notBefore", APR_HASH_KEY_STRING);
    apr_hash_set(tgt, "notBefore", APR_HASH_KEY_STRING, date_str);

    date_str = apr_hash_get(sectrans_cert->content, "notAfter", APR_HASH_KEY_STRING);
    apr_hash_set(tgt, "notAfter", APR_HASH_KEY_STRING, date_str);

    sha1 = apr_hash_get(sectrans_cert->content, "sha1", APR_HASH_KEY_STRING);
    apr_hash_set(tgt, "sha1", APR_HASH_KEY_STRING, sha1);
    serf__log(SSL_VERBOSE, __FILE__, "SHA1 fingerprint:%s.\n", sha1);

    /* TODO: array of subjectAltName's */

    return tgt;
}


/* Functions to read a serf_ssl_certificate structure. */
int cert_depth(const serf_ssl_certificate_t *cert)
{
    serf__log(SSL_VERBOSE, __FILE__,
              "TODO: function cert_depth not implemented.\n");

    return 0;
}

apr_hash_t *cert_issuer(const serf_ssl_certificate_t *cert,
                        apr_pool_t *pool)
{
    sectrans_certificate_t *sectrans_cert = cert->impl_cert;

    if (!sectrans_cert->content) {
        apr_status_t status;
        status = serf__sectrans_read_X509_DER_certificate(&sectrans_cert->content,
                                                          sectrans_cert,
                                                          pool);
        if (status)
            return NULL;
    }

    return (apr_hash_t *)apr_hash_get(sectrans_cert->content,
                                      "issuer", APR_HASH_KEY_STRING);
}

apr_hash_t *cert_subject(const serf_ssl_certificate_t *cert,
                         apr_pool_t *pool)
{
    sectrans_certificate_t *sectrans_cert = cert->impl_cert;

    if (!sectrans_cert->content) {
        apr_status_t status;
        status = serf__sectrans_read_X509_DER_certificate(&sectrans_cert->content,
                                                 sectrans_cert,
                                                 pool);
        if (status)
            return NULL;
    }

    return (apr_hash_t *)apr_hash_get(sectrans_cert->content,
                                      "subject", APR_HASH_KEY_STRING);
}

const char *cert_export(const serf_ssl_certificate_t *cert,
                        apr_pool_t *pool)
{
    sectrans_certificate_t *sectrans_cert = cert->impl_cert;
    SecCertificateRef certref = sectrans_cert->certref;
    CFDataRef dataref = SecCertificateCopyData(certref);
    const unsigned char *data = CFDataGetBytePtr(dataref);

    CFIndex len = CFDataGetLength(dataref);

    if (!len)
        return NULL;

    char *encoded_cert = apr_palloc(pool, apr_base64_encode_len(len));

    apr_base64_encode(encoded_cert, (char*)data, len);

    return encoded_cert;
}

static apr_status_t
use_compression(void *impl_ctx, int enabled)
{
    if (enabled) {
        serf__log(SSL_VERBOSE, __FILE__,
                  "Secure Transport does not support any type of "
                  "SSL compression.\n");
        return APR_ENOTIMPL;
    } else {
        return APR_SUCCESS;
    }
}

int set_allowed_cert_validation_modes(void *impl_ctx,
                                      int modes)
{
    sectrans_context_t *ssl_ctx = impl_ctx;

    ssl_ctx->modes = 0;

    if (modes & serf_ssl_val_mode_serf_managed_with_gui)
        ssl_ctx->modes |= serf_ssl_val_mode_serf_managed_with_gui;
    if (modes & serf_ssl_val_mode_serf_managed_no_gui)
        ssl_ctx->modes |= serf_ssl_val_mode_serf_managed_no_gui;
    if (modes & serf_ssl_val_mode_application_managed)
        ssl_ctx->modes |= serf_ssl_val_mode_application_managed;

    return ssl_ctx->modes;
}

/**** ENCRYPTION BUCKET API *****/
/********************************/
static apr_status_t
serf_sectrans_encrypt_read(serf_bucket_t *bucket,
                           apr_size_t requested,
                           const char **data, apr_size_t *len)
{
    sectrans_context_t *ssl_ctx = bucket->data;
    apr_status_t status, status_unenc_stream;
    const char *unenc_data;
    size_t unenc_len;
    
    serf__log(SSL_VERBOSE, __FILE__, "serf_sectrans_encrypt_read called for "
              "%d bytes.\n", requested);

    /* Pending handshake? */
    if (ssl_ctx->state == SERF_SECTRANS_INIT ||
        ssl_ctx->state == SERF_SECTRANS_HANDSHAKE)
    {
        ssl_ctx->state = SERF_SECTRANS_HANDSHAKE;
        status = do_handshake(ssl_ctx);

        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        if (!status)
        {
            serf__log(SSL_VERBOSE, __FILE__, "ssl/tls handshake successful.\n");
            ssl_ctx->state = SERF_SECTRANS_CONNECTED;
        } else {
            /* Maybe the handshake algorithm put some data in the pending
               outgoing bucket? */
            return serf_bucket_read(ssl_ctx->encrypt.pending, requested, data, len);
        }
    }

    /* Handshake successful. */

    /* First use any pending encrypted data. */
    status = serf_bucket_read(ssl_ctx->encrypt.pending, requested, data, len);
    if (SERF_BUCKET_READ_ERROR(status))
        return status;

    if (*len) {
        /* status can be either APR_EAGAIN or APR_SUCCESS. In both cases,
           we want the caller to try again as there's probably more data
           to be encrypted. */
        return APR_SUCCESS;
    }

    /* Encrypt more data. */
    status_unenc_stream = serf_bucket_read(ssl_ctx->encrypt.stream, requested,
                                           &unenc_data, &unenc_len);
    if (SERF_BUCKET_READ_ERROR(status_unenc_stream))
        return status_unenc_stream;

    if (unenc_len)
    {
        OSStatus sectrans_status;
        size_t written;

        /* TODO: we now feed each individual chunk of data one by one to 
           SSLWrite. This seems to add a record header etc. per call, 
           so 2 bytes of data in results in 37 bytes of data out.
           Need to add a real buffer and feed this function chunks of
           e.g. 8KB. */
        sectrans_status = SSLWrite(ssl_ctx->st_ctxr, unenc_data, unenc_len,
                                   &written);
        status = translate_sectrans_status(sectrans_status);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        serf__log(SSL_MSG_VERBOSE, __FILE__, "%dB ready with status %d, %d encrypted and written:\n"
                  "---%.*s-(%d)-\n", unenc_len, status_unenc_stream, written, written, unenc_data, written);

        status = serf_bucket_read(ssl_ctx->encrypt.pending, requested,
                                  data, len);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        /* Tell the caller there's more data readily available. */
        if (status == APR_SUCCESS)
            return status;
    }

    /* All encrypted data was returned, if there's more available depends
       on what's pending on the to-be-encrypted stream. */
    return status_unenc_stream;
}

static apr_status_t
serf_sectrans_encrypt_readline(serf_bucket_t *bucket,
                               int acceptable, int *found,
                               const char **data,
                               apr_size_t *len)
{
    serf__log(SSL_VERBOSE, __FILE__,
              "function serf_sectrans_encrypt_readline not implemented.\n");
    return APR_ENOTIMPL;
}


static apr_status_t
serf_sectrans_encrypt_peek(serf_bucket_t *bucket,
                           const char **data,
                           apr_size_t *len)
{
    sectrans_context_t *ssl_ctx = bucket->data;

    return serf_bucket_peek(ssl_ctx->encrypt.pending, data, len);
}

static void
serf_sectrans_encrypt_destroy_and_data(serf_bucket_t *bucket)
{
    sectrans_context_t *ssl_ctx = bucket->data;

    if (!--ssl_ctx->refcount) {
        sectrans_free_context(ssl_ctx, bucket->allocator);
    }

    serf_bucket_ssl_destroy_and_data(bucket);
}

/**** DECRYPTION BUCKET API *****/
/********************************/
static apr_status_t
serf_sectrans_decrypt_peek(serf_bucket_t *bucket,
                           const char **data,
                           apr_size_t *len)
{
    sectrans_context_t *ssl_ctx = bucket->data;
    
    return serf_bucket_peek(ssl_ctx->decrypt.pending, data, len);
}

/* Ask Secure Transport to decrypt some more data. If anything was received,
   add it to the to decrypt.pending buffer.
 */
static apr_status_t
decrypt_more_data(sectrans_context_t *ssl_ctx)
{
    /* Decrypt more data. */
    serf_bucket_t *tmp;
    char *dec_data;
    size_t dec_len;
    OSStatus sectrans_status;
    apr_status_t status;

    serf__log(SSL_VERBOSE, __FILE__,
              "decrypt_more_data called.\n");

    /* We have to provide ST with the buffer for the decrypted data. */
    dec_data = serf_bucket_mem_alloc(ssl_ctx->decrypt.pending->allocator,
                                     SECURE_TRANSPORT_READ_BUFSIZE);

    sectrans_status = SSLRead(ssl_ctx->st_ctxr, dec_data,
                              SECURE_TRANSPORT_READ_BUFSIZE,
                              &dec_len);
    status = translate_sectrans_status(sectrans_status);
    if (SERF_BUCKET_READ_ERROR(status))
        return status;

    /* Successfully received and decrypted some data, add to pending. */
    serf__log(SSL_MSG_VERBOSE, __FILE__, " received and decrypted data:"
              "---\n%.*s\n-(%d)-\n", dec_len, dec_data, dec_len);

    tmp = SERF_BUCKET_SIMPLE_STRING_LEN(dec_data, dec_len,
                                        ssl_ctx->decrypt.pending->allocator);
    serf_bucket_aggregate_append(ssl_ctx->decrypt.pending, tmp);

    return status;
}

static apr_status_t
serf_sectrans_decrypt_read(serf_bucket_t *bucket,
                           apr_size_t requested,
                           const char **data, apr_size_t *len)
{
    sectrans_context_t *ssl_ctx = bucket->data;
    apr_status_t status;

    serf__log(SSL_VERBOSE, __FILE__,
              "serf_sectrans_decrypt_read called for %d bytes.\n", requested);

    /* First use any pending encrypted data. */
    status = serf_bucket_read(ssl_ctx->decrypt.pending,
                              requested, data, len);
    if (SERF_BUCKET_READ_ERROR(status))
        return status;

    if (*len)
        return status;

    /* TODO: integrate this loop in decrypt_more_data so we can be more 
       efficient with memory. */
    do {
        /* Pending buffer empty, decrypt more. */
        status = decrypt_more_data(ssl_ctx);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;
    } while (status == APR_SUCCESS);

    /* We should now have more decrypted data in the pending buffer. */
    return serf_bucket_read(ssl_ctx->decrypt.pending, requested, data,
                            len);
}

/* TODO: remove some logging to make the function easier to read. */
static apr_status_t
serf_sectrans_decrypt_readline(serf_bucket_t *bucket,
                               int acceptable, int *found,
                               const char **data,
                               apr_size_t *len)
{
    sectrans_context_t *ssl_ctx = bucket->data;
    apr_status_t status;

    serf__log(SSL_VERBOSE, __FILE__,
              "serf_sectrans_decrypt_readline called.\n");

    /* First use any pending encrypted data. */
    status = serf_bucket_readline(ssl_ctx->decrypt.pending, acceptable, found,
                                  data, len);
    if (SERF_BUCKET_READ_ERROR(status))
        goto error;

    if (*len) {
        serf__log(SSL_VERBOSE, __FILE__, "  read one %s line.\n",
                  *found ? "complete" : "partial");
        return status;
    }

    do {
        /* Pending buffer empty, decrypt more. */
        status = decrypt_more_data(ssl_ctx);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;
    } while (status == APR_SUCCESS);

    /* We have more decrypted data in the pending buffer. */
    status = serf_bucket_readline(ssl_ctx->decrypt.pending, acceptable, found,
                                  data, len);
    if (SERF_BUCKET_READ_ERROR(status))
        goto error;

    serf__log(SSL_VERBOSE, __FILE__, "  read one %s line.\n",
              *found ? "complete" : "partial");
    return status;

error:
    serf__log(SSL_VERBOSE, __FILE__, "  return with status %d.\n", status);
    return status;
}

static void
serf_sectrans_decrypt_destroy_and_data(serf_bucket_t *bucket)
{
    sectrans_context_t *ssl_ctx = bucket->data;

    if (!--ssl_ctx->refcount) {
        sectrans_free_context(ssl_ctx, bucket->allocator);
    }

    serf_bucket_ssl_destroy_and_data(bucket);
}

const serf_bucket_type_t serf_bucket_type_sectrans_encrypt = {
    "SECURETRANSPORTENCRYPT",
    serf_sectrans_encrypt_read,
    serf_sectrans_encrypt_readline,
    serf_default_read_iovec,
    serf_default_read_for_sendfile,
    serf_default_read_bucket,
    serf_sectrans_encrypt_peek,
    serf_sectrans_encrypt_destroy_and_data,
};

const serf_bucket_type_t    serf_bucket_type_sectrans_decrypt = {
    "SECURETRANSPORTDECRYPT",
    serf_sectrans_decrypt_read,
    serf_sectrans_decrypt_readline,
    serf_default_read_iovec,
    serf_default_read_for_sendfile,
    serf_default_read_bucket,
    serf_sectrans_decrypt_peek,
    serf_sectrans_decrypt_destroy_and_data,
};

const serf_ssl_bucket_type_t serf_ssl_bucket_type_securetransport = {
    decrypt_create,
    decrypt_context_get,
    encrypt_create,
    encrypt_context_get,
    set_hostname,
    client_cert_provider_set,
    client_cert_password_set,
    server_cert_callback_set,
    server_cert_chain_callback_set,
    use_default_certificates,
    load_CA_cert_from_file,
    trust_cert,
    cert_issuer,
    cert_subject,
    cert_certificate,
    cert_export,
    use_compression,
    set_allowed_cert_validation_modes,
};

#endif /* SERF_HAVE_SECURETRANSPORT */