/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2010 OpenVPN Technologies, Inc. <sales@openvpn.net>
 *  Copyright (C) 2010 Fox Crypto B.V. <openvpn@fox-it.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file Control Channel OpenSSL Backend
 */

#include "syshead.h"

#if defined(USE_SSL) && defined(USE_OPENSSL)

#include "errlevel.h"
#include "buffer.h"
#include "misc.h"
#include "manage.h"
#include "memdbg.h"
#include "ssl_backend.h"
#include "ssl_common.h"
#include "base64.h"

#ifdef ENABLE_CRYPTOAPI
#include "cryptoapi.h"
#endif

#include "ssl_verify_openssl.h"

#include <openssl/err.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>
#include <openssl/crypto.h>

/*
 * Allocate space in SSL objects in which to store a struct tls_session
 * pointer back to parent.
 *
 */

int mydata_index; /* GLOBAL */

void
tls_init_lib()
{
  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms ();

  mydata_index = SSL_get_ex_new_index(0, "struct session *", NULL, NULL, NULL);
  ASSERT (mydata_index >= 0);
}

void
tls_free_lib()
{
  EVP_cleanup();
  ERR_free_strings();
}

void
tls_clear_error()
{
  ERR_clear_error ();
}

/*
 * OpenSSL callback to get a temporary RSA key, mostly
 * used for export ciphers.
 */
static RSA *
tmp_rsa_cb (SSL * s, int is_export, int keylength)
{
  static RSA *rsa_tmp = NULL;
  if (rsa_tmp == NULL)
    {
      msg (D_HANDSHAKE, "Generating temp (%d bit) RSA key", keylength);
      rsa_tmp = RSA_generate_key (keylength, RSA_F4, NULL, NULL);
    }
  return (rsa_tmp);
}

void
tls_ctx_server_new(struct tls_root_ctx *ctx)
{
  ASSERT(NULL != ctx);

  ctx->ctx = SSL_CTX_new (TLSv1_server_method ());

  if (ctx->ctx == NULL)
    msg (M_SSLERR, "SSL_CTX_new TLSv1_server_method");

  SSL_CTX_set_tmp_rsa_callback (ctx->ctx, tmp_rsa_cb);
}

void
tls_ctx_client_new(struct tls_root_ctx *ctx)
{
  ASSERT(NULL != ctx);

  ctx->ctx = SSL_CTX_new (TLSv1_client_method ());

  if (ctx->ctx == NULL)
    msg (M_SSLERR, "SSL_CTX_new TLSv1_client_method");
}

void
tls_ctx_free(struct tls_root_ctx *ctx)
{
  ASSERT(NULL != ctx);
  if (NULL != ctx->ctx)
    SSL_CTX_free (ctx->ctx);
  ctx->ctx = NULL;
}

bool tls_ctx_initialised(struct tls_root_ctx *ctx)
{
  ASSERT(NULL != ctx);
  return NULL != ctx->ctx;
}

/*
 * Print debugging information on SSL/TLS session negotiation.
 */

#ifndef INFO_CALLBACK_SSL_CONST
#define INFO_CALLBACK_SSL_CONST const
#endif
static void
info_callback (INFO_CALLBACK_SSL_CONST SSL * s, int where, int ret)
{
  if (where & SSL_CB_LOOP)
    {
      dmsg (D_HANDSHAKE_VERBOSE, "SSL state (%s): %s",
	   where & SSL_ST_CONNECT ? "connect" :
	   where & SSL_ST_ACCEPT ? "accept" :
	   "undefined", SSL_state_string_long (s));
    }
  else if (where & SSL_CB_ALERT)
    {
      dmsg (D_HANDSHAKE_VERBOSE, "SSL alert (%s): %s: %s",
	   where & SSL_CB_READ ? "read" : "write",
	   SSL_alert_type_string_long (ret),
	   SSL_alert_desc_string_long (ret));
    }
}

void
tls_ctx_set_options (struct tls_root_ctx *ctx, unsigned int ssl_flags)
{
  ASSERT(NULL != ctx);

  SSL_CTX_set_session_cache_mode (ctx->ctx, SSL_SESS_CACHE_OFF);
  SSL_CTX_set_options (ctx->ctx, SSL_OP_SINGLE_DH_USE);
  SSL_CTX_set_default_passwd_cb (ctx->ctx, pem_password_callback);

  /* Require peer certificate verification */
#if P2MP_SERVER
  if (ssl_flags & SSLF_CLIENT_CERT_NOT_REQUIRED)
    {
      msg (M_WARN, "WARNING: POTENTIALLY DANGEROUS OPTION "
	  "--client-cert-not-required may accept clients which do not present "
	  "a certificate");
    }
  else
#endif
  SSL_CTX_set_verify (ctx->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
		      verify_callback);

  SSL_CTX_set_info_callback (ctx->ctx, info_callback);
}

void
tls_ctx_restrict_ciphers(struct tls_root_ctx *ctx, const char *ciphers)
{
  ASSERT(NULL != ctx);

  if(!SSL_CTX_set_cipher_list(ctx->ctx, ciphers))
    msg(M_SSLERR, "Failed to set restricted TLS cipher list: %s", ciphers);
}

void
tls_ctx_load_dh_params (struct tls_root_ctx *ctx, const char *dh_file
#if ENABLE_INLINE_FILES
    , const char *dh_file_inline
#endif /* ENABLE_INLINE_FILES */
    )
{
  DH *dh;
  BIO *bio;

  ASSERT(NULL != ctx);

#if ENABLE_INLINE_FILES
  if (!strcmp (dh_file, INLINE_FILE_TAG) && dh_file_inline)
    {
      if (!(bio = BIO_new_mem_buf ((char *)dh_file_inline, -1)))
	msg (M_SSLERR, "Cannot open memory BIO for inline DH parameters");
    }
  else
#endif /* ENABLE_INLINE_FILES */
    {
      /* Get Diffie Hellman Parameters */
      if (!(bio = BIO_new_file (dh_file, "r")))
	msg (M_SSLERR, "Cannot open %s for DH parameters", dh_file);
    }

  dh = PEM_read_bio_DHparams (bio, NULL, NULL, NULL);
  BIO_free (bio);

  if (!dh)
    msg (M_SSLERR, "Cannot load DH parameters from %s", dh_file);
  if (!SSL_CTX_set_tmp_dh (ctx->ctx, dh))
    msg (M_SSLERR, "SSL_CTX_set_tmp_dh");

  msg (D_TLS_DEBUG_LOW, "Diffie-Hellman initialized with %d bit key",
       8 * DH_size (dh));

  DH_free (dh);
}

int
tls_ctx_load_pkcs12(struct tls_root_ctx *ctx, const char *pkcs12_file,
#if ENABLE_INLINE_FILES
    const char *pkcs12_file_inline,
#endif /* ENABLE_INLINE_FILES */
    bool load_ca_file
    )
{
  FILE *fp;
  EVP_PKEY *pkey;
  X509 *cert;
  STACK_OF(X509) *ca = NULL;
  PKCS12 *p12;
  int i;
  char password[256];

  ASSERT(NULL != ctx);

#if ENABLE_INLINE_FILES
  if (!strcmp (pkcs12_file, INLINE_FILE_TAG) && pkcs12_file_inline)
    {
      BIO *b64 = BIO_new(BIO_f_base64());
      BIO *bio = BIO_new_mem_buf((void *) pkcs12_file_inline,
	  (int) strlen(pkcs12_file_inline));
      ASSERT(b64 && bio);
      BIO_push(b64, bio);
      p12 = d2i_PKCS12_bio(b64, NULL);
      if (!p12)
	msg(M_SSLERR, "Error reading inline PKCS#12 file");
      BIO_free(b64);
      BIO_free(bio);
    }
  else
#endif
    {
      /* Load the PKCS #12 file */
      if (!(fp = fopen(pkcs12_file, "rb")))
	msg(M_SSLERR, "Error opening file %s", pkcs12_file);
      p12 = d2i_PKCS12_fp(fp, NULL);
      fclose(fp);
      if (!p12)
	msg(M_SSLERR, "Error reading PKCS#12 file %s", pkcs12_file);
    }

  /* Parse the PKCS #12 file */
  if (!PKCS12_parse(p12, "", &pkey, &cert, &ca))
   {
     pem_password_callback (password, sizeof(password) - 1, 0, NULL);
     /* Reparse the PKCS #12 file with password */
     ca = NULL;
     if (!PKCS12_parse(p12, password, &pkey, &cert, &ca))
      {
#ifdef ENABLE_MANAGEMENT
	      if (management && (ERR_GET_REASON (ERR_peek_error()) == PKCS12_R_MAC_VERIFY_FAILURE))
		management_auth_failure (management, UP_TYPE_PRIVATE_KEY, NULL);
#endif
	PKCS12_free(p12);
	return 1;
      }
   }
  PKCS12_free(p12);

  /* Load Certificate */
  if (!SSL_CTX_use_certificate (ctx->ctx, cert))
   msg (M_SSLERR, "Cannot use certificate");

  /* Load Private Key */
  if (!SSL_CTX_use_PrivateKey (ctx->ctx, pkey))
   msg (M_SSLERR, "Cannot use private key");
  warn_if_group_others_accessible (pkcs12_file);

  /* Check Private Key */
  if (!SSL_CTX_check_private_key (ctx->ctx))
   msg (M_SSLERR, "Private key does not match the certificate");

  /* Set Certificate Verification chain */
  if (load_ca_file)
   {
     if (ca && sk_X509_num(ca))
      {
	for (i = 0; i < sk_X509_num(ca); i++)
	  {
	      if (!X509_STORE_add_cert(ctx->ctx->cert_store,sk_X509_value(ca, i)))
	      msg (M_SSLERR, "Cannot add certificate to certificate chain (X509_STORE_add_cert)");
	    if (!SSL_CTX_add_client_CA(ctx->ctx, sk_X509_value(ca, i)))
	      msg (M_SSLERR, "Cannot add certificate to client CA list (SSL_CTX_add_client_CA)");
	  }
      }
   }
  return 0;
}

#ifdef ENABLE_CRYPTOAPI
void
tls_ctx_load_cryptoapi(struct tls_root_ctx *ctx, const char *cryptoapi_cert)
{
  ASSERT(NULL != ctx);

  /* Load Certificate and Private Key */
  if (!SSL_CTX_use_CryptoAPI_certificate (ctx->ctx, cryptoapi_cert))
    msg (M_SSLERR, "Cannot load certificate \"%s\" from Microsoft Certificate Store",
	   cryptoapi_cert);
}
#endif /* WIN32 */

/*
 * Based on SSL_CTX_use_certificate_file, return an x509 object for the
 * given file.
 */
static int
tls_ctx_read_certificate_file(SSL_CTX *ctx, const char *file, X509 **x509)
{
  BIO *in;
  int ret=0;
  X509 *x=NULL;

  in=BIO_new(BIO_s_file_internal());
  if (in == NULL)
    {
      SSLerr(SSL_F_SSL_CTX_USE_CERTIFICATE_FILE, ERR_R_BUF_LIB);
      goto end;
    }

  if (BIO_read_filename(in,file) <= 0)
    {
      SSLerr(SSL_F_SSL_CTX_USE_CERTIFICATE_FILE, ERR_R_SYS_LIB);
      goto end;
    }

  x = PEM_read_bio_X509(in, NULL, ctx->default_passwd_callback,
    ctx->default_passwd_callback_userdata);
  if (x == NULL)
    {
      SSLerr(SSL_F_SSL_CTX_USE_CERTIFICATE_FILE, ERR_R_PEM_LIB);
      goto end;
    }

 end:
  if (in != NULL)
    BIO_free(in);
  if (x509)
    *x509 = x;
  else if (x)
    X509_free (x);
  return(ret);
}

void
tls_ctx_load_cert_file (struct tls_root_ctx *ctx, const char *cert_file,
#if ENABLE_INLINE_FILES
    const char *cert_file_inline,
#endif
    X509 **x509
    )
{
  ASSERT(NULL != ctx);
  if (NULL != x509)
    ASSERT(NULL == *x509);

#if ENABLE_INLINE_FILES
  if (!strcmp (cert_file, INLINE_FILE_TAG) && cert_file_inline)
    {
      BIO *in = NULL;
      X509 *x = NULL;
      int ret = 0;

      in = BIO_new_mem_buf ((char *)cert_file_inline, -1);
      if (in)
	{
	  x = PEM_read_bio_X509 (in,
			     NULL,
			     ctx->ctx->default_passwd_callback,
			     ctx->ctx->default_passwd_callback_userdata);
	  BIO_free (in);
	  if (x) {
	    ret = SSL_CTX_use_certificate(ctx->ctx, x);
	    if (x509)
	      *x509 = x;
	    else
	      X509_free (x);
	  }
	}

      if (!ret)
        msg (M_SSLERR, "Cannot load inline certificate file");
    }
  else
#endif /* ENABLE_INLINE_FILES */
    {
      if (!SSL_CTX_use_certificate_chain_file (ctx->ctx, cert_file))
	msg (M_SSLERR, "Cannot load certificate file %s", cert_file);
      if (x509)
	{
	  if (!tls_ctx_read_certificate_file(ctx->ctx, cert_file, x509))
	    msg (M_SSLERR, "Cannot load certificate file %s", cert_file);
	}
    }
}

void
tls_ctx_free_cert_file (X509 *x509)
{
  X509_free(x509);
}

#if ENABLE_INLINE_FILES
static int
use_inline_PrivateKey_file (SSL_CTX *ctx, const char *key_string)
{
  BIO *in = NULL;
  EVP_PKEY *pkey = NULL;
  int ret = 0;

  in = BIO_new_mem_buf ((char *)key_string, -1);
  if (!in)
    goto end;

  pkey = PEM_read_bio_PrivateKey (in,
				  NULL,
				  ctx->default_passwd_callback,
				  ctx->default_passwd_callback_userdata);
  if (!pkey)
    goto end;

  ret = SSL_CTX_use_PrivateKey (ctx, pkey);

 end:
  if (pkey)
    EVP_PKEY_free (pkey);
  if (in)
    BIO_free (in);
  return ret;
}
#endif /* ENABLE_INLINE_FILES */

int
tls_ctx_load_priv_file (struct tls_root_ctx *ctx, const char *priv_key_file
#if ENABLE_INLINE_FILES
    , const char *priv_key_file_inline
#endif
    )
{
  int status;

  ASSERT(NULL != ctx);

#if ENABLE_INLINE_FILES
  if (!strcmp (priv_key_file, INLINE_FILE_TAG) && priv_key_file_inline)
    {
      status = use_inline_PrivateKey_file (ctx->ctx, priv_key_file_inline);
    }
  else
#endif /* ENABLE_INLINE_FILES */
    {
      status = SSL_CTX_use_PrivateKey_file (ctx->ctx, priv_key_file, SSL_FILETYPE_PEM);
    }
  if (!status)
    {
#ifdef ENABLE_MANAGEMENT
      if (management && (ERR_GET_REASON (ERR_peek_error()) == EVP_R_BAD_DECRYPT))
	  management_auth_failure (management, UP_TYPE_PRIVATE_KEY, NULL);
#endif
      msg (M_WARN|M_SSL, "Cannot load private key file %s", priv_key_file);
      return 1;
    }
  warn_if_group_others_accessible (priv_key_file);

  /* Check Private Key */
  if (!SSL_CTX_check_private_key (ctx->ctx))
    msg (M_SSLERR, "Private key does not match the certificate");
  return 0;

}

#ifdef MANAGMENT_EXTERNAL_KEY

/* encrypt */
static int
rsa_pub_enc(int flen, const unsigned char *from, unsigned char *to, RSA *rsa, int padding)
{
  ASSERT(0);
  return -1;
}

/* verify arbitrary data */
static int
rsa_pub_dec(int flen, const unsigned char *from, unsigned char *to, RSA *rsa, int padding)
{
  ASSERT(0);
  return -1;
}

/* decrypt */
static int
rsa_priv_dec(int flen, const unsigned char *from, unsigned char *to, RSA *rsa, int padding)
{
  ASSERT(0);
  return -1;
}

/* called at RSA_free */
static int
rsa_finish(RSA *rsa)
{
  free ((void*)rsa->meth);
  rsa->meth = NULL;
  return 1;
}

/* sign arbitrary data */
static int
rsa_priv_enc(int flen, const unsigned char *from, unsigned char *to, RSA *rsa, int padding)
{
  /* optional app data in rsa->meth->app_data; */
  char *in_b64 = NULL;
  char *out_b64 = NULL;
  int ret = -1;
  int len;

  if (padding != RSA_PKCS1_PADDING)
    {
      RSAerr (RSA_F_RSA_EAY_PRIVATE_ENCRYPT, RSA_R_UNKNOWN_PADDING_TYPE);
      goto done;
    }

  /* convert 'from' to base64 */
  if (openvpn_base64_encode (from, flen, &in_b64) <= 0)
    goto done;

  /* call MI for signature */
  if (management)
    out_b64 = management_query_rsa_sig (management, in_b64);
  if (!out_b64)
    goto done;

  /* decode base64 signature to binary */
  len = RSA_size(rsa);
  ret = openvpn_base64_decode (out_b64, to, len);

  /* verify length */
  if (ret != len)
    ret = -1;

 done:
  if (in_b64)
    free (in_b64);
  if (out_b64)
    free (out_b64);
  return ret;
}

int
tls_ctx_use_external_private_key (struct tls_root_ctx *ctx, X509 *cert)
{
  RSA *rsa = NULL;
  RSA *pub_rsa;
  RSA_METHOD *rsa_meth;

  ASSERT (NULL != ctx);
  ASSERT (NULL != cert);

  /* allocate custom RSA method object */
  ALLOC_OBJ_CLEAR (rsa_meth, RSA_METHOD);
  rsa_meth->name = "OpenVPN external private key RSA Method";
  rsa_meth->rsa_pub_enc = rsa_pub_enc;
  rsa_meth->rsa_pub_dec = rsa_pub_dec;
  rsa_meth->rsa_priv_enc = rsa_priv_enc;
  rsa_meth->rsa_priv_dec = rsa_priv_dec;
  rsa_meth->init = NULL;
  rsa_meth->finish = rsa_finish;
  rsa_meth->flags = RSA_METHOD_FLAG_NO_CHECK;
  rsa_meth->app_data = NULL;

  /* allocate RSA object */
  rsa = RSA_new();
  if (rsa == NULL)
    {
      SSLerr(SSL_F_SSL_USE_PRIVATEKEY, ERR_R_MALLOC_FAILURE);
      goto err;
    }

  /* get the public key */
  ASSERT(cert->cert_info->key->pkey); /* NULL before SSL_CTX_use_certificate() is called */
  pub_rsa = cert->cert_info->key->pkey->pkey.rsa;

  /* initialize RSA object */
  rsa->n = BN_dup(pub_rsa->n);
  rsa->flags |= RSA_FLAG_EXT_PKEY;
  if (!RSA_set_method(rsa, rsa_meth))
    goto err;

  /* bind our custom RSA object to ssl_ctx */
  if (!SSL_CTX_use_RSAPrivateKey(ctx->ctx, rsa))
    goto err;

  RSA_free(rsa); /* doesn't necessarily free, just decrements refcount */
  return 1;

 err:
  if (rsa)
    RSA_free(rsa);
  else
    {
      if (rsa_meth)
	free(rsa_meth);
    }
  msg (M_SSLERR, "Cannot enable SSL external private key capability");
  return 0;
}

#endif

#if ENABLE_INLINE_FILES
static int
xname_cmp(const X509_NAME * const *a, const X509_NAME * const *b)
{
  return(X509_NAME_cmp(*a,*b));
}

static STACK_OF(X509_NAME) *
use_inline_load_client_CA_file (SSL_CTX *ctx, const char *ca_string)
{
  BIO *in = NULL;
  X509 *x = NULL;
  X509_NAME *xn = NULL;
  STACK_OF(X509_NAME) *ret = NULL, *sk;

  sk=sk_X509_NAME_new(xname_cmp);

  in = BIO_new_mem_buf ((char *)ca_string, -1);
  if (!in)
    goto err;

  if ((sk == NULL) || (in == NULL))
    goto err;

  for (;;)
    {
      if (PEM_read_bio_X509(in,&x,NULL,NULL) == NULL)
	break;
      if (ret == NULL)
	{
	  ret = sk_X509_NAME_new_null();
	  if (ret == NULL)
	    goto err;
	}
      if ((xn=X509_get_subject_name(x)) == NULL) goto err;
      /* check for duplicates */
      xn=X509_NAME_dup(xn);
      if (xn == NULL) goto err;
      if (sk_X509_NAME_find(sk,xn) >= 0)
	X509_NAME_free(xn);
      else
	{
	  sk_X509_NAME_push(sk,xn);
	  sk_X509_NAME_push(ret,xn);
	}
    }

  if (0)
    {
    err:
      if (ret != NULL) sk_X509_NAME_pop_free(ret,X509_NAME_free);
      ret=NULL;
    }
  if (sk != NULL) sk_X509_NAME_free(sk);
  if (in != NULL) BIO_free(in);
  if (x != NULL) X509_free(x);
  if (ret != NULL)
    ERR_clear_error();
  return(ret);
}

static int
use_inline_load_verify_locations (SSL_CTX *ctx, const char *ca_string)
{
  X509_STORE *store = NULL;
  X509* cert = NULL;
  BIO *in = NULL;
  int ret = 0;

  in = BIO_new_mem_buf ((char *)ca_string, -1);
  if (!in)
    goto err;

  for (;;)
    {
      if (!PEM_read_bio_X509 (in, &cert, 0, NULL))
	{
	  ret = 1;
	  break;
	}
      if (!cert)
	break;

      store = SSL_CTX_get_cert_store (ctx);
      if (!store)
	break;

      if (!X509_STORE_add_cert (store, cert))
	break;

      if (cert)
	{
	  X509_free (cert);
	  cert = NULL;
	}
    }

 err:
  if (cert)
    X509_free (cert);
  if (in)
    BIO_free (in);
  return ret;
}
#endif /* ENABLE_INLINE_FILES */

void
tls_ctx_load_ca (struct tls_root_ctx *ctx, const char *ca_file,
#if ENABLE_INLINE_FILES
    const char *ca_file_inline,
#endif
    const char *ca_path, bool tls_server
    )
{
  int status;

  ASSERT(NULL != ctx);

#if ENABLE_INLINE_FILES
  if (ca_file && !strcmp (ca_file, INLINE_FILE_TAG) && ca_file_inline)
	{
	  status = use_inline_load_verify_locations (ctx->ctx, ca_file_inline);
	}
  else
#endif
	{
	  /* Load CA file for verifying peer supplied certificate */
	  status = SSL_CTX_load_verify_locations (ctx->ctx, ca_file, ca_path);
	}

  if (!status)
	msg (M_SSLERR, "Cannot load CA certificate file %s path %s (SSL_CTX_load_verify_locations)", np(ca_file), np(ca_path));

  /* Set a store for certs (CA & CRL) with a lookup on the "capath" hash directory */
  if (ca_path) {
    X509_STORE *store = SSL_CTX_get_cert_store(ctx->ctx);

    if (store)
	  {
	    X509_LOOKUP *lookup = X509_STORE_add_lookup(store, X509_LOOKUP_hash_dir());
	    if (!X509_LOOKUP_add_dir(lookup, ca_path, X509_FILETYPE_PEM))
	      X509_LOOKUP_add_dir(lookup, NULL, X509_FILETYPE_DEFAULT);
	    else
	      msg(M_WARN, "WARNING: experimental option --capath %s", ca_path);
#if OPENSSL_VERSION_NUMBER >= 0x00907000L
	    X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
#else
	    msg(M_WARN, "WARNING: this version of OpenSSL cannot handle CRL files in capath");
#endif
	  }
	else
      msg(M_SSLERR, "Cannot get certificate store (SSL_CTX_get_cert_store)");
  }

  /* Load names of CAs from file and use it as a client CA list */
  if (ca_file && tls_server) {
    STACK_OF(X509_NAME) *cert_names = NULL;
#if ENABLE_INLINE_FILES
	if (!strcmp (ca_file, INLINE_FILE_TAG) && ca_file_inline)
	  {
	    cert_names = use_inline_load_client_CA_file (ctx->ctx, ca_file_inline);
	  }
	else
#endif
	  {
	    cert_names = SSL_load_client_CA_file (ca_file);
	  }
    if (!cert_names)
      msg (M_SSLERR, "Cannot load CA certificate file %s (SSL_load_client_CA_file)", ca_file);
    SSL_CTX_set_client_CA_list (ctx->ctx, cert_names);
  }
}


void
tls_ctx_load_extra_certs (struct tls_root_ctx *ctx, const char *extra_certs_file
#if ENABLE_INLINE_FILES
    , const char *extra_certs_file_inline
#endif
    )
{
  BIO *bio;
  X509 *cert;
#if ENABLE_INLINE_FILES
  if (!strcmp (extra_certs_file, INLINE_FILE_TAG) && extra_certs_file_inline)
    {
      bio = BIO_new_mem_buf ((char *)extra_certs_file_inline, -1);
    }
  else
#endif
    {
      bio = BIO_new(BIO_s_file());
      if (BIO_read_filename(bio, extra_certs_file) <= 0)
	msg (M_SSLERR, "Cannot load extra-certs file: %s", extra_certs_file);
    }
  for (;;)
    {
      cert = NULL;
      if (!PEM_read_bio_X509 (bio, &cert, 0, NULL)) /* takes ownership of cert */
	break;
      if (!cert)
	msg (M_SSLERR, "Error reading extra-certs certificate");
      if (SSL_CTX_add_extra_chain_cert(ctx->ctx, cert) != 1)
	msg (M_SSLERR, "Error adding extra-certs certificate");
    }
  BIO_free (bio);
}

/* **************************************
 *
 * Key-state specific functions
 *
 ***************************************/
/*
 *
 * BIO functions
 *
 */

#ifdef BIO_DEBUG

#warning BIO_DEBUG defined

static FILE *biofp;                            /* GLOBAL */
static bool biofp_toggle;                      /* GLOBAL */
static time_t biofp_last_open;                 /* GLOBAL */
static const int biofp_reopen_interval = 600;  /* GLOBAL */

static void
close_biofp()
{
  if (biofp)
    {
      ASSERT (!fclose (biofp));
      biofp = NULL;
    }
}

static void
open_biofp()
{
  const time_t current = time (NULL);
  const pid_t pid = getpid ();

  if (biofp_last_open + biofp_reopen_interval < current)
    close_biofp();
  if (!biofp)
    {
      char fn[256];
      openvpn_snprintf(fn, sizeof(fn), "bio/%d-%d.log", pid, biofp_toggle);
      biofp = fopen (fn, "w");
      ASSERT (biofp);
      biofp_last_open = time (NULL);
      biofp_toggle ^= 1;
    }
}

static void
bio_debug_data (const char *mode, BIO *bio, const uint8_t *buf, int len, const char *desc)
{
  struct gc_arena gc = gc_new ();
  if (len > 0)
    {
      open_biofp();
      fprintf(biofp, "BIO_%s %s time=" time_format " bio=" ptr_format " len=%d data=%s\n",
	      mode, desc, time (NULL), (ptr_type)bio, len, format_hex (buf, len, 0, &gc));
      fflush (biofp);
    }
  gc_free (&gc);
}

static void
bio_debug_oc (const char *mode, BIO *bio)
{
  open_biofp();
  fprintf(biofp, "BIO %s time=" time_format " bio=" ptr_format "\n",
	  mode, time (NULL), (ptr_type)bio);
  fflush (biofp);
}

#endif

/*
 * OpenVPN's interface to SSL/TLS authentication,
 * encryption, and decryption is exclusively
 * through "memory BIOs".
 */
static BIO *
getbio (BIO_METHOD * type, const char *desc)
{
  BIO *ret;
  ret = BIO_new (type);
  if (!ret)
    msg (M_SSLERR, "Error creating %s BIO", desc);
  return ret;
}

/*
 * Write to an OpenSSL BIO in non-blocking mode.
 */
static int
bio_write (BIO *bio, const uint8_t *data, int size, const char *desc)
{
  int i;
  int ret = 0;
  ASSERT (size >= 0);
  if (size)
    {
      /*
       * Free the L_TLS lock prior to calling BIO routines
       * so that foreground thread can still call
       * tls_pre_decrypt or tls_pre_encrypt,
       * allowing tunnel packet forwarding to continue.
       */
#ifdef BIO_DEBUG
      bio_debug_data ("write", bio, data, size, desc);
#endif
      i = BIO_write (bio, data, size);

      if (i < 0)
	{
	  if (BIO_should_retry (bio))
	    {
	      ;
	    }
	  else
	    {
	      msg (D_TLS_ERRORS | M_SSL, "TLS ERROR: BIO write %s error",
		   desc);
	      ret = -1;
	      ERR_clear_error ();
	    }
	}
      else if (i != size)
	{
	  msg (D_TLS_ERRORS | M_SSL,
	       "TLS ERROR: BIO write %s incomplete %d/%d", desc, i, size);
	  ret = -1;
	  ERR_clear_error ();
	}
      else
	{			/* successful write */
	  dmsg (D_HANDSHAKE_VERBOSE, "BIO write %s %d bytes", desc, i);
	  ret = 1;
	}
    }
  return ret;
}

/*
 * Inline functions for reading from and writing
 * to BIOs.
 */

static void
bio_write_post (const int status, struct buffer *buf)
{
  if (status == 1) /* success status return from bio_write? */
    {
      memset (BPTR (buf), 0, BLEN (buf)); /* erase data just written */
      buf->len = 0;
    }
}

/*
 * Read from an OpenSSL BIO in non-blocking mode.
 */
static int
bio_read (BIO *bio, struct buffer *buf, int maxlen, const char *desc)
{
  int i;
  int ret = 0;
  ASSERT (buf->len >= 0);
  if (buf->len)
    {
      ;
    }
  else
    {
      int len = buf_forward_capacity (buf);
      if (maxlen < len)
	len = maxlen;

      /*
       * BIO_read brackets most of the serious RSA
       * key negotiation number crunching.
       */
      i = BIO_read (bio, BPTR (buf), len);

      VALGRIND_MAKE_READABLE ((void *) &i, sizeof (i));

#ifdef BIO_DEBUG
      bio_debug_data ("read", bio, BPTR (buf), i, desc);
#endif
      if (i < 0)
	{
	  if (BIO_should_retry (bio))
	    {
	      ;
	    }
	  else
	    {
	      msg (D_TLS_ERRORS | M_SSL, "TLS_ERROR: BIO read %s error",
		   desc);
	      buf->len = 0;
	      ret = -1;
	      ERR_clear_error ();
	    }
	}
      else if (!i)
	{
	  buf->len = 0;
	}
      else
	{			/* successful read */
	  dmsg (D_HANDSHAKE_VERBOSE, "BIO read %s %d bytes", desc, i);
	  buf->len = i;
	  ret = 1;
	  VALGRIND_MAKE_READABLE ((void *) BPTR (buf), BLEN (buf));
	}
    }
  return ret;
}

void
key_state_ssl_init(struct key_state_ssl *ks_ssl, const struct tls_root_ctx *ssl_ctx, bool is_server, void *session)
{
  ASSERT(NULL != ssl_ctx);
  ASSERT(ks_ssl);
  CLEAR (*ks_ssl);

  ks_ssl->ssl = SSL_new (ssl_ctx->ctx);
  if (!ks_ssl->ssl)
    msg (M_SSLERR, "SSL_new failed");

  /* put session * in ssl object so we can access it
     from verify callback*/
  SSL_set_ex_data (ks_ssl->ssl, mydata_index, session);

  ks_ssl->ssl_bio = getbio (BIO_f_ssl (), "ssl_bio");
  ks_ssl->ct_in = getbio (BIO_s_mem (), "ct_in");
  ks_ssl->ct_out = getbio (BIO_s_mem (), "ct_out");

#ifdef BIO_DEBUG
  bio_debug_oc ("open ssl_bio", ks_ssl->ssl_bio);
  bio_debug_oc ("open ct_in", ks_ssl->ct_in);
  bio_debug_oc ("open ct_out", ks_ssl->ct_out);
#endif

  if (is_server)
    SSL_set_accept_state (ks_ssl->ssl);
  else
    SSL_set_connect_state (ks_ssl->ssl);

  SSL_set_bio (ks_ssl->ssl, ks_ssl->ct_in, ks_ssl->ct_out);
  BIO_set_ssl (ks_ssl->ssl_bio, ks_ssl->ssl, BIO_NOCLOSE);
}

void key_state_ssl_free(struct key_state_ssl *ks_ssl)
{
  if (ks_ssl->ssl) {
#ifdef BIO_DEBUG
    bio_debug_oc ("close ssl_bio", ks_ssl->ssl_bio);
    bio_debug_oc ("close ct_in", ks_ssl->ct_in);
    bio_debug_oc ("close ct_out", ks_ssl->ct_out);
#endif
    BIO_free_all(ks_ssl->ssl_bio);
    SSL_free (ks_ssl->ssl);
  }
}

int
key_state_write_plaintext (struct key_state_ssl *ks_ssl, struct buffer *buf)
{
  int ret = 0;
  perf_push (PERF_BIO_WRITE_PLAINTEXT);

#ifdef USE_OPENSSL
  ASSERT (NULL != ks_ssl);

  ret = bio_write (ks_ssl->ssl_bio, BPTR(buf), BLEN(buf),
      "tls_write_plaintext");
  bio_write_post (ret, buf);
#endif /* USE_OPENSSL */

  perf_pop ();
  return ret;
}

int
key_state_write_plaintext_const (struct key_state_ssl *ks_ssl, const uint8_t *data, int len)
{
  int ret = 0;
  perf_push (PERF_BIO_WRITE_PLAINTEXT);

  ASSERT (NULL != ks_ssl);

  ret = bio_write (ks_ssl->ssl_bio, data, len, "tls_write_plaintext_const");

  perf_pop ();
  return ret;
}

int
key_state_read_ciphertext (struct key_state_ssl *ks_ssl, struct buffer *buf,
    int maxlen)
{
  int ret = 0;
  perf_push (PERF_BIO_READ_CIPHERTEXT);

  ASSERT (NULL != ks_ssl);

  ret = bio_read (ks_ssl->ct_out, buf, maxlen, "tls_read_ciphertext");

  perf_pop ();
  return ret;
}

int
key_state_write_ciphertext (struct key_state_ssl *ks_ssl, struct buffer *buf)
{
  int ret = 0;
  perf_push (PERF_BIO_WRITE_CIPHERTEXT);

  ASSERT (NULL != ks_ssl);

  ret = bio_write (ks_ssl->ct_in, BPTR(buf), BLEN(buf), "tls_write_ciphertext");
  bio_write_post (ret, buf);

  perf_pop ();
  return ret;
}

int
key_state_read_plaintext (struct key_state_ssl *ks_ssl, struct buffer *buf,
    int maxlen)
{
  int ret = 0;
  perf_push (PERF_BIO_READ_PLAINTEXT);

  ASSERT (NULL != ks_ssl);

  ret = bio_read (ks_ssl->ssl_bio, buf, maxlen, "tls_read_plaintext");

  perf_pop ();
  return ret;
}

/* **************************************
 *
 * Information functions
 *
 * Print information for the end user.
 *
 ***************************************/
void
print_details (struct key_state_ssl * ks_ssl, const char *prefix)
{
  const SSL_CIPHER *ciph;
  X509 *cert;
  char s1[256];
  char s2[256];

  s1[0] = s2[0] = 0;
  ciph = SSL_get_current_cipher (ks_ssl->ssl);
  openvpn_snprintf (s1, sizeof (s1), "%s %s, cipher %s %s",
		    prefix,
		    SSL_get_version (ks_ssl->ssl),
		    SSL_CIPHER_get_version (ciph),
		    SSL_CIPHER_get_name (ciph));
  cert = SSL_get_peer_certificate (ks_ssl->ssl);
  if (cert != NULL)
    {
      EVP_PKEY *pkey = X509_get_pubkey (cert);
      if (pkey != NULL)
	{
	  if (pkey->type == EVP_PKEY_RSA && pkey->pkey.rsa != NULL
	      && pkey->pkey.rsa->n != NULL)
	    {
	      openvpn_snprintf (s2, sizeof (s2), ", %d bit RSA",
				BN_num_bits (pkey->pkey.rsa->n));
	    }
	  else if (pkey->type == EVP_PKEY_DSA && pkey->pkey.dsa != NULL
		   && pkey->pkey.dsa->p != NULL)
	    {
	      openvpn_snprintf (s2, sizeof (s2), ", %d bit DSA",
				BN_num_bits (pkey->pkey.dsa->p));
	    }
	  EVP_PKEY_free (pkey);
	}
      X509_free (cert);
    }
  /* The SSL API does not allow us to look at temporary RSA/DH keys,
   * otherwise we should print their lengths too */
  msg (D_HANDSHAKE, "%s%s", s1, s2);
}

void
show_available_tls_ciphers ()
{
  SSL_CTX *ctx;
  SSL *ssl;
  const char *cipher_name;
  int priority = 0;

  ctx = SSL_CTX_new (TLSv1_method ());
  if (!ctx)
    msg (M_SSLERR, "Cannot create SSL_CTX object");

  ssl = SSL_new (ctx);
  if (!ssl)
    msg (M_SSLERR, "Cannot create SSL object");

  printf ("Available TLS Ciphers,\n");
  printf ("listed in order of preference:\n\n");
  while ((cipher_name = SSL_get_cipher_list (ssl, priority++)))
    printf ("%s\n", cipher_name);
  printf ("\n");

  SSL_free (ssl);
  SSL_CTX_free (ctx);
}

void
get_highest_preference_tls_cipher (char *buf, int size)
{
  SSL_CTX *ctx;
  SSL *ssl;
  const char *cipher_name;

  ctx = SSL_CTX_new (TLSv1_method ());
  if (!ctx)
    msg (M_SSLERR, "Cannot create SSL_CTX object");
  ssl = SSL_new (ctx);
  if (!ssl)
    msg (M_SSLERR, "Cannot create SSL object");

  cipher_name = SSL_get_cipher_list (ssl, 0);
  strncpynt (buf, cipher_name, size);

  SSL_free (ssl);
  SSL_CTX_free (ctx);
}

#endif /* defined(USE_SSL) && defined(USE_OPENSSL) */
