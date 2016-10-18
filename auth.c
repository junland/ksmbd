/*
 *   fs/cifssrv/auth.c
 *
 *   Copyright (C) 2015 Samsung Electronics Co., Ltd.
 *   Copyright (C) 2016 Namjae Jeon <namjae.jeon@protocolfreedom.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/backing-dev.h>
#include <linux/writeback.h>
#include <linux/xattr.h>

#include "glob.h"
#include "export.h"

/* Fixed format data defining GSS header and fixed string
 * "not_defined_in_RFC4178@please_ignore".
 * So sec blob data in neg phase could be generated statically.
 */
char NEGOTIATE_GSS_HEADER[74] =  {
	0x60, 0x48, 0x06, 0x06, 0x2b, 0x06, 0x01, 0x05,
	0x05, 0x02, 0xa0, 0x3e, 0x30, 0x3c, 0xa0, 0x0e,
	0x30, 0x0c, 0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04,
	0x01, 0x82, 0x37, 0x02, 0x02, 0x0a, 0xa3, 0x2a,
	0x30, 0x28, 0xa0, 0x26, 0x1b, 0x24, 0x6e, 0x6f,
	0x74, 0x5f, 0x64, 0x65, 0x66, 0x69, 0x6e, 0x65,
	0x64, 0x5f, 0x69, 0x6e, 0x5f, 0x52, 0x46, 0x43,
	0x34, 0x31, 0x37, 0x38, 0x40, 0x70, 0x6c, 0x65,
	0x61, 0x73, 0x65, 0x5f, 0x69, 0x67, 0x6e, 0x6f,
	0x72, 0x65
};

char SESSION_NEGOTIATE_GSS_HEADER[31] = {
	0xa1, 0x81, 0xbe, 0x30, 0x81, 0xbb, 0xa0, 0x03,
	0x0a, 0x01, 0x01, 0xa1, 0x0c, 0x06, 0x0a, 0x2b,
	0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02,
	0x0a, 0xa2, 0x81, 0xa5, 0x04, 0x81, 0xa2
};

char SESSION_AUTHENTICATE_GSS_HEADER[9] = {
	0xa1, 0x07, 0x30, 0x05, 0xa0, 0x03, 0x0a, 0x01,
	0x00
};

static int crypto_md5_alloc(struct tcp_server_info *server)
{
	int rc;
	unsigned int size;

	/* check if already allocated */
	if (server->secmech.md5)
		return 0;

	server->secmech.md5 = crypto_alloc_shash("md5", 0, 0);
	if (IS_ERR(server->secmech.md5)) {
		cifssrv_debug("could not allocate crypto md5\n");
		rc = PTR_ERR(server->secmech.md5);
		server->secmech.md5 = NULL;
		return rc;
	}

	size = sizeof(struct shash_desc) +
		crypto_shash_descsize(server->secmech.md5);
	server->secmech.sdescmd5 = kmalloc(size, GFP_KERNEL);
	if (!server->secmech.sdescmd5) {
		crypto_free_shash(server->secmech.md5);
		server->secmech.md5 = NULL;
		return -ENOMEM;
	}
	server->secmech.sdescmd5->shash.tfm = server->secmech.md5;
	server->secmech.sdescmd5->shash.flags = 0x0;

	return 0;
}

static int crypto_hmacmd5_alloc(struct tcp_server_info *server)
{
	int rc;
	unsigned int size;

	/* check if already allocated */
	if (server->secmech.sdeschmacmd5)
		return 0;

	server->secmech.hmacmd5 = crypto_alloc_shash("hmac(md5)", 0, 0);
	if (IS_ERR(server->secmech.hmacmd5)) {
		cifssrv_debug("could not allocate crypto hmacmd5\n");
		rc = PTR_ERR(server->secmech.hmacmd5);
		server->secmech.hmacmd5 = NULL;
		return rc;
	}

	size = sizeof(struct shash_desc) +
		crypto_shash_descsize(server->secmech.hmacmd5);
	server->secmech.sdeschmacmd5 = kmalloc(size, GFP_KERNEL);
	if (!server->secmech.sdeschmacmd5) {
		crypto_free_shash(server->secmech.hmacmd5);
		server->secmech.hmacmd5 = NULL;
		return -ENOMEM;
	}
	server->secmech.sdeschmacmd5->shash.tfm = server->secmech.hmacmd5;
	server->secmech.sdeschmacmd5->shash.flags = 0x0;

	return 0;
}

/**
 * compute_sess_key() - function to generate session key
 * @sess:	session of connection
 * @hash:	source hash value to be used for find session key
 * @hmac:	source hmac value to be used for finding session key
 *
 */
int compute_sess_key(struct cifssrv_sess *sess, char *hash, char *hmac)
{
	int rc;

	rc = crypto_hmacmd5_alloc(sess->server);
	if (rc) {
		cifssrv_debug("could not crypto alloc hmacmd5 rc %d\n", rc);
		goto out;
	}

	rc = crypto_shash_setkey(sess->server->secmech.hmacmd5, hash,
			CIFS_HMAC_MD5_HASH_SIZE);
	if (rc) {
		cifssrv_debug("hmacmd5 set key fail error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_init(&sess->server->secmech.sdeschmacmd5->shash);
	if (rc) {
		cifssrv_debug("could not init hmacmd5 error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_update(&sess->server->secmech.sdeschmacmd5->shash,
		hmac, SMB2_NTLMV2_SESSKEY_SIZE);
	if (rc) {
		cifssrv_debug("Could not update with response error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_final(&sess->server->secmech.sdeschmacmd5->shash,
			sess->sess_key);
	if (rc) {
		cifssrv_debug("Could not generate hmacmd5 hash error %d\n", rc);
		goto out;
	}

out:
	return rc;
}

static int calc_ntlmv2_hash(struct cifssrv_sess *sess, char *ntlmv2_hash,
	char *dname)
{
	int ret, len;
	wchar_t *domain;
	__le16 *uniname;

	if (!sess->server->secmech.sdeschmacmd5) {
		cifssrv_debug("can't generate ntlmv2 hash\n");
		return -1;
	}

	ret = crypto_shash_setkey(sess->server->secmech.hmacmd5,
		sess->usr->passkey, CIFS_ENCPWD_SIZE);
	if (ret) {
		cifssrv_debug("Could not set NT Hash as a key\n");
		return ret;
	}

	ret = crypto_shash_init(&sess->server->secmech.sdeschmacmd5->shash);
	if (ret) {
		cifssrv_debug("could not init hmacmd5\n");
		return ret;
	}

	/* convert user_name to unicode */
	len = strlen(sess->usr->name);
	uniname = kzalloc(2 + UNICODE_LEN(len), GFP_KERNEL);
	if (!uniname) {
		ret = -ENOMEM;
		return ret;
	}

	if (len) {
		len = smb_strtoUTF16(uniname, sess->usr->name, len,
			sess->server->local_nls);
		UniStrupr(uniname);
	}

	ret = crypto_shash_update(&sess->server->secmech.sdeschmacmd5->shash,
			(char *)uniname, UNICODE_LEN(len));
	if (ret) {
		cifssrv_debug("Could not update with user\n");
		return ret;
	}

	/* Convert domain name or server name to unicode and uppercase */
	len = strlen(dname);
	domain = kzalloc(2 + UNICODE_LEN(len), GFP_KERNEL);
	if (!domain) {
		cifssrv_debug("memory allocation failed\n");
		ret = -ENOMEM;
		return ret;
	}

	len = smb_strtoUTF16((__le16 *)domain, dname, len,
		sess->server->local_nls);

	ret = crypto_shash_update(&sess->server->secmech.sdeschmacmd5->shash,
					(char *)domain, UNICODE_LEN(len));
	if (ret) {
		cifssrv_debug("Could not update with domain\n");
		return ret;
	}

	ret = crypto_shash_final(&sess->server->secmech.sdeschmacmd5->shash,
			ntlmv2_hash);
	if (ret) {
		cifssrv_debug("Could not generate md5 hash\n");
	}

	return ret;
}

/**
 * process_ntlmv() - NTLM authentication handler
 * @sess:	session of connection
 * @pw_buf:	NTLM challenge response
 * @passkey:	user password
 *
 * Return:	0 on success, error number on error
 */
int process_ntlm(struct cifssrv_sess *sess, char *pw_buf)
{
	int rc;
	unsigned char p21[21];
	char key[CIFS_AUTH_RESP_SIZE];

	memset(p21, '\0', 21);
	memcpy(p21, sess->usr->passkey, CIFS_NTHASH_SIZE);
	rc = E_P24(p21, sess->ntlmssp.cryptkey, key);
	if (rc) {
		cifssrv_err("password processing failed\n");
		return rc;
	}

	smb_mdfour(sess->sess_key, sess->usr->passkey, CIFS_SMB1_SESSKEY_SIZE);
	memcpy(sess->sess_key + CIFS_SMB1_SESSKEY_SIZE, key,
		CIFS_AUTH_RESP_SIZE);
	sess->sequence_number = 1;

	if (strncmp(pw_buf, key, CIFS_AUTH_RESP_SIZE) != 0) {
		cifssrv_debug("ntlmv1 authentication failed\n");
		rc = -EINVAL;
	} else
		cifssrv_debug("ntlmv1 authentication pass\n");

	return rc;
}

/**
 * process_ntlmv2() - NTLMv2 authentication handler
 * @sess:	session of connection
 * @ntlmv2:		NTLMv2 challenge response
 * @blen:		NTLMv2 blob length
 * @domain_name:	domain name
 *
 * Return:	0 on success, error number on error
 */
int process_ntlmv2(struct cifssrv_sess *sess, struct ntlmv2_resp *ntlmv2,
		int blen, char *domain_name)
{
	char ntlmv2_hash[CIFS_ENCPWD_SIZE];
	char ntlmv2_rsp[CIFS_HMAC_MD5_HASH_SIZE];
	char *construct;
	int rc, len;

	rc = crypto_hmacmd5_alloc(sess->server);
	if (rc) {
		cifssrv_debug("could not crypto alloc hmacmd5 rc %d\n", rc);
		goto out;
	}

	if (domain_name == netbios_name)
		rc = calc_ntlmv2_hash(sess, ntlmv2_hash, netbios_name);
	else
		rc = calc_ntlmv2_hash(sess, ntlmv2_hash, domain_name);

	if (rc) {
		cifssrv_debug("could not get v2 hash rc %d\n", rc);
		goto out;
	}

	rc = crypto_shash_setkey(sess->server->secmech.hmacmd5, ntlmv2_hash,
						CIFS_HMAC_MD5_HASH_SIZE);
	if (rc) {
		cifssrv_debug("Could not set NTLMV2 Hash as a key\n");
		goto out;
	}

	rc = crypto_shash_init(&sess->server->secmech.sdeschmacmd5->shash);
	if (rc) {
		cifssrv_debug("Could not init hmacmd5\n");
		goto out;
	}

	len = CIFS_CRYPTO_KEY_SIZE + blen;
	construct = kzalloc(len, GFP_KERNEL);
	if (!construct) {
		cifssrv_debug("Memory allocation failed\n");
		rc = -ENOMEM;
		goto out;
	}

	memcpy(construct, sess->ntlmssp.cryptkey, CIFS_CRYPTO_KEY_SIZE);
	memcpy(construct + CIFS_CRYPTO_KEY_SIZE,
		(char *)(&ntlmv2->blob_signature), blen);

	rc = crypto_shash_update(&sess->server->secmech.sdeschmacmd5->shash,
			construct, len);
	if (rc) {
		cifssrv_debug("Could not update with response\n");
		goto out;
	}

	rc = crypto_shash_final(&sess->server->secmech.sdeschmacmd5->shash,
			ntlmv2_rsp);
	if (rc) {
		cifssrv_debug("Could not generate md5 hash\n");
		goto out;
	}

	rc = compute_sess_key(sess, ntlmv2_hash, ntlmv2_rsp);
	if (rc) {
		cifssrv_debug("%s: Could not generate sess key\n", __func__);
		goto out;
	}

	rc = memcmp(ntlmv2->ntlmv2_hash, ntlmv2_rsp, CIFS_HMAC_MD5_HASH_SIZE);
out:
	return rc;
}

/**
 * decode_ntlmssp_authenticate_blob() - helper function to construct
 * authenticate blob
 * @authblob:	authenticate blob source pointer
 * @usr:	user details
 * @sess:	session of connection
 *
 * Return:	0 on success, error number on error
 */
int decode_ntlmssp_authenticate_blob(AUTHENTICATE_MESSAGE *authblob,
	int blob_len, struct cifssrv_sess *sess)
{
	char *domain_name;

	if (blob_len < sizeof(AUTHENTICATE_MESSAGE)) {
		cifssrv_debug("negotiate blob len %d too small\n", blob_len);
		return -EINVAL;
	}

	if (memcmp(authblob->Signature, "NTLMSSP", 8)) {
		cifssrv_debug("blob signature incorrect %s\n",
				authblob->Signature);
		return -EINVAL;
	}

	/* process NTLM authentication */
	if (authblob->NtChallengeResponse.Length == CIFS_AUTH_RESP_SIZE) {
		return process_ntlm(sess, (char *)authblob +
			authblob->NtChallengeResponse.BufferOffset);
	}

	/* TODO : use domain name that imported from configuration file */
	domain_name = smb_strndup_from_utf16(
			(const char *)authblob +
			authblob->DomainName.BufferOffset,
			authblob->DomainName.Length, true,
			sess->server->local_nls);

	/* process NTLMv2 authentication */
	cifssrv_debug("decode_ntlmssp_authenticate_blob dname%s\n",
			domain_name);
	return process_ntlmv2(sess, (struct ntlmv2_resp *)((char *)authblob +
		authblob->NtChallengeResponse.BufferOffset),
		authblob->NtChallengeResponse.Length - CIFS_ENCPWD_SIZE,
		domain_name);
}

/**
 * decode_ntlmssp_negotiate_blob() - helper function to construct negotiate blob
 * @negblob:	negotiate blob source pointer
 * @rsp:	response header pointer to be updated
 * @sess:	session of connection
 *
 */
int decode_ntlmssp_negotiate_blob(NEGOTIATE_MESSAGE *negblob,
		int blob_len, struct cifssrv_sess *sess)
{
	if (blob_len < sizeof(NEGOTIATE_MESSAGE)) {
		cifssrv_debug("negotiate blob len %d too small\n", blob_len);
		return -EINVAL;
	}

	if (memcmp(negblob->Signature, "NTLMSSP", 8)) {
		cifssrv_debug("blob signature incorrect %s\n",
				negblob->Signature);
		return -EINVAL;
	}

	sess->ntlmssp.client_flags = negblob->NegotiateFlags;
	return 0;
}

/**
 * build_ntlmssp_challenge_blob() - helper function to construct challenge blob
 * @chgblob:	challenge blob source pointer to initialize
 * @rsp:	response header pointer to be updated
 * @sess:	session of connection
 *
 */
unsigned int build_ntlmssp_challenge_blob(CHALLENGE_MESSAGE *chgblob,
		struct cifssrv_sess *sess)
{
	TargetInfo *tinfo;
	__le16 name[8];
	__u8 *target_name;
	unsigned int len, flags, blob_len, type;

	memcpy(chgblob->Signature, NTLMSSP_SIGNATURE, 8);
	chgblob->MessageType = NtLmChallenge;

	flags = NTLMSSP_NEGOTIATE_UNICODE |
		NTLMSSP_NEGOTIATE_NTLM | NTLMSSP_TARGET_TYPE_SERVER |
		NTLMSSP_NEGOTIATE_TARGET_INFO |
		NTLMSSP_NEGOTIATE_128 | NTLMSSP_NEGOTIATE_56 |
		NTLMSSP_NEGOTIATE_VERSION;

	if (sess->ntlmssp.client_flags & NTLMSSP_REQUEST_TARGET)
		flags |= NTLMSSP_REQUEST_TARGET;

	chgblob->NegotiateFlags = cpu_to_le32(flags);

	len = smb_strtoUTF16(name, netbios_name, strlen(netbios_name),
			sess->server->local_nls);
	len = UNICODE_LEN(len);
	chgblob->TargetName.Length = cpu_to_le16(len);
	chgblob->TargetName.MaximumLength = cpu_to_le16(len);
	chgblob->TargetName.BufferOffset =
			cpu_to_le32(sizeof(CHALLENGE_MESSAGE));

	/* Initialize random server challenge */
	get_random_bytes(sess->ntlmssp.cryptkey, sizeof(__u64));
	memcpy(chgblob->Challenge, sess->ntlmssp.cryptkey,
		CIFS_CRYPTO_KEY_SIZE);

	/* Add Target Information to security buffer */
	chgblob->TargetInfoArray.BufferOffset =
		chgblob->TargetName.BufferOffset + len;

	target_name = (__u8 *)chgblob + chgblob->TargetName.BufferOffset;
	memcpy(target_name, name, len);
	blob_len = cpu_to_le16(sizeof(CHALLENGE_MESSAGE) + len);
	tinfo = (TargetInfo *)(target_name + len);

	chgblob->TargetInfoArray.Length = 0;
	/* Add target info list for NetBIOS/DNS settings */
	for (type = NTLMSSP_AV_NB_COMPUTER_NAME;
		type <= NTLMSSP_AV_DNS_DOMAIN_NAME; type++) {
		tinfo->Type = type;
		tinfo->Length = len;
		memcpy(tinfo->Content, name, len);
		tinfo = (TargetInfo *)((char *)tinfo + 4 + len);
		chgblob->TargetInfoArray.Length += cpu_to_le16(4 + len);
	}

	/* Add terminator subblock */
	tinfo->Type = 0;
	tinfo->Length = 0;
	chgblob->TargetInfoArray.Length += cpu_to_le16(4);

	chgblob->TargetInfoArray.MaximumLength =
			chgblob->TargetInfoArray.Length;
	blob_len += chgblob->TargetInfoArray.Length;
	cifssrv_debug("NTLMSSP SecurityBufferLength %d\n", blob_len);
	return blob_len;
}

/**
 * smb1_sign_smbpdu() - function to generate SMB1 packet signing
 * @sess:	session of connection
 * @buf:        source pointer to client request packet
 * @sz:         size of client request packet
 * @sig:        signature value generated for client request packet
 *
 */
int smb1_sign_smbpdu(struct cifssrv_sess *sess, char *buf, int sz,
		char *sig)
{
	int rc;

	rc = crypto_md5_alloc(sess->server);
	if (rc) {
		cifssrv_debug("could not crypto alloc md5 rc %d\n", rc);
		goto out;
	}

	rc = crypto_shash_init(&sess->server->secmech.sdescmd5->shash);
	if (rc) {
		cifssrv_debug("md5 init error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_update(&sess->server->secmech.sdescmd5->shash,
			sess->sess_key, 40);
	if (rc) {
		cifssrv_debug("md5 update error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_update(&sess->server->secmech.sdescmd5->shash,
			buf, sz);
	if (rc) {
		cifssrv_debug("md5 update error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_final(&sess->server->secmech.sdescmd5->shash, sig);
	if (rc)
		cifssrv_debug("md5 generation error %d\n", rc);

out:
	return rc;
}

#ifdef CONFIG_CIFS_SMB2_SERVER

static int crypto_hmacsha256_alloc(struct tcp_server_info *server)
{
	int rc;
	unsigned int size;

	/* check if already allocated */
	if (server->secmech.hmacsha256)
		return 0;

	server->secmech.hmacsha256 = crypto_alloc_shash("hmac(sha256)", 0, 0);
	if (IS_ERR(server->secmech.hmacsha256)) {
		cifssrv_debug("could not allocate crypto hmacsha256\n");
		rc = PTR_ERR(server->secmech.hmacsha256);
		server->secmech.hmacsha256 = NULL;
		return rc;
	}

	size = sizeof(struct shash_desc) +
		crypto_shash_descsize(server->secmech.hmacsha256);
	server->secmech.sdeschmacsha256 = kmalloc(size, GFP_KERNEL);
	if (!server->secmech.sdeschmacsha256) {
		crypto_free_shash(server->secmech.hmacsha256);
		server->secmech.hmacsha256 = NULL;
		return -ENOMEM;
	}
	server->secmech.sdeschmacsha256->shash.tfm = server->secmech.hmacsha256;
	server->secmech.sdeschmacsha256->shash.flags = 0x0;

	return 0;
}

static int crypto_cmac_alloc(struct tcp_server_info *server)
{
	int rc;
	unsigned int size;

	/* check if already allocated */
	if (server->secmech.sdesccmacaes)
		return 0;

	server->secmech.cmacaes = crypto_alloc_shash("cmac(aes)", 0, 0);
	if (IS_ERR(server->secmech.cmacaes)) {
		cifssrv_debug("could not allocate crypto cmac-aes\n");
		rc = PTR_ERR(server->secmech.cmacaes);
		server->secmech.cmacaes = NULL;
		return rc;
	}

	size = sizeof(struct shash_desc) +
		crypto_shash_descsize(server->secmech.cmacaes);
	server->secmech.sdesccmacaes = kmalloc(size, GFP_KERNEL);
	if (!server->secmech.sdesccmacaes) {
		crypto_free_shash(server->secmech.cmacaes);
		server->secmech.cmacaes = NULL;
		return -ENOMEM;
	}
	server->secmech.sdesccmacaes->shash.tfm = server->secmech.cmacaes;
	server->secmech.sdesccmacaes->shash.flags = 0x0;

	return 0;
}

static int crypto_sha512_alloc(struct tcp_server_info *server)
{
	int rc;
	unsigned int size;

	/* check if already allocated */
	if (server->secmech.sdescsha512)
		return 0;

	cifssrv_debug("Inside crypto_sha512_alloc\n");
	server->secmech.sha512 = crypto_alloc_shash("sha512", 0, 0);
	if (IS_ERR(server->secmech.sha512)) {
		cifssrv_debug("could not allocate crypto sha512\n");
		rc = PTR_ERR(server->secmech.sha512);
		server->secmech.sha512 = NULL;
		return rc;
	}

	size = sizeof(struct shash_desc) +
		crypto_shash_descsize(server->secmech.sha512);
	server->secmech.sdescsha512 = kmalloc(size, GFP_KERNEL);
	if (!server->secmech.sdescsha512) {
		crypto_free_shash(server->secmech.sha512);
		server->secmech.sha512 = NULL;
		return -ENOMEM;
	}
	server->secmech.sdescsha512->shash.tfm = server->secmech.sha512;
	server->secmech.sdescsha512->shash.flags = 0x0;

	return 0;
}

/**
 * smb2_sign_smbpdu() - function to generate packet signing
 * @sess:	session of connection
 * @buf:	source pointer to client request packet
 * @sz:		size of client request packet
 * @sig:	signature value generated for client request packet
 *
 */
int smb2_sign_smbpdu(struct cifssrv_sess *sess, char *buf, int sz,
		char *sig)
{
	int rc;

	rc = crypto_hmacsha256_alloc(sess->server);
	if (rc) {
		cifssrv_debug("could not crypto alloc hmacmd5 rc %d\n", rc);
		goto out;
	}

	rc = crypto_shash_setkey(sess->server->secmech.hmacsha256,
		sess->sess_key, SMB2_NTLMV2_SESSKEY_SIZE);
	if (rc) {
		cifssrv_debug("hmacsha256 update error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_init(&sess->server->secmech.sdeschmacsha256->shash);
	if (rc) {
		cifssrv_debug("hmacsha256 init error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_update(&sess->server->secmech.sdeschmacsha256->shash,
					buf, sz);
	if (rc) {
		cifssrv_debug("hmacsha256 update error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_final(&sess->server->secmech.sdeschmacsha256->shash,
		sig);
	if (rc)
		cifssrv_debug("hmacsha256 generation error %d\n", rc);

out:
	return rc;
}

/**
 * smb3_sign_smbpdu() - function to generate packet signing
 * @sess:	session of connection
 * @buf:	source pointer to client request packet
 * @sz:		size of client request packet
 * @sig:	signature value generated for client request packet
 *
 */
int smb3_sign_smbpdu(struct channel *chann, char *buf, int sz,
		char *sig)
{
	int rc;

	rc = crypto_shash_setkey(chann->server->secmech.cmacaes,
		chann->smb3signingkey,	SMB2_CMACAES_SIZE);
	if (rc) {
		cifssrv_debug("cmaces update error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_init(&chann->server->secmech.sdesccmacaes->shash);
	if (rc) {
		cifssrv_debug("cmaces init error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_update(&chann->server->secmech.sdesccmacaes->shash,
					buf, sz);
	if (rc) {
		cifssrv_debug("cmaces update error %d\n", rc);
		goto out;
	}

	rc = crypto_shash_final(&chann->server->secmech.sdesccmacaes->shash,
		sig);
	if (rc)
		cifssrv_debug("cmaces generation error %d\n", rc);

out:
	return rc;
}


/**
 * compute_smb3xsigningkey() - function to generate session key
 * @sess:	session of connection
 *
 */
int compute_smb3xsigningkey(struct cifssrv_sess *sess, __u8 *key,
	unsigned int key_size)
{
	unsigned char zero = 0x0;
	int rc;
	__u8 i[4] = {0, 0, 0, 1};
	__u8 L[4] = {0, 0, 0, 128};
	unsigned char prfhash[SMB2_HMACSHA256_SIZE];
	unsigned char *hashptr = prfhash;

	memset(prfhash, 0x0, SMB2_HMACSHA256_SIZE);
	memset(key, 0x0, key_size);

	rc = crypto_hmacsha256_alloc(sess->server);
	if (rc) {
		cifssrv_debug("could not crypto alloc hmacmd5 rc %d\n", rc);
		goto smb3signkey_ret;
	}

	rc = crypto_cmac_alloc(sess->server);
	if (rc) {
		cifssrv_debug("could not crypto alloc cmac rc %d\n", rc);
		goto smb3signkey_ret;
	}

	rc = crypto_shash_setkey(sess->server->secmech.hmacsha256,
			sess->sess_key, SMB2_NTLMV2_SESSKEY_SIZE);
	if (rc) {
		cifssrv_debug("could not set with session key\n");
		goto smb3signkey_ret;
	}

	rc = crypto_shash_init(&sess->server->secmech.sdeschmacsha256->shash);
	if (rc) {
		cifssrv_debug("could not init sign hmac\n");
		goto smb3signkey_ret;
	}

	rc = crypto_shash_update(&sess->server->secmech.sdeschmacsha256->shash,
			i, 4);
	if (rc) {
		cifssrv_debug("could not update with n\n");
		goto smb3signkey_ret;
	}

	if (sess->server->dialect == SMB311_PROT_ID)
		rc = crypto_shash_update(&sess->server->
				secmech.sdeschmacsha256->shash,
				"SMBSigningKey", 14);
	else
		rc = crypto_shash_update(&sess->server->
				secmech.sdeschmacsha256->shash,
				"SMB2AESCMAC", 12);
	if (rc) {
		cifssrv_debug("could not update with label\n");
		goto smb3signkey_ret;
	}

	rc = crypto_shash_update(&sess->server->secmech.sdeschmacsha256->shash,
			&zero, 1);
	if (rc) {
		cifssrv_debug("could not update with zero\n");
		goto smb3signkey_ret;
	}

	if (sess->server->dialect == SMB311_PROT_ID)
		rc = crypto_shash_update(&sess->server->
			secmech.sdeschmacsha256->shash, sess->Preauth_HashValue,
			64);
	else
		rc = crypto_shash_update(&sess->server->
			secmech.sdeschmacsha256->shash, "SmbSign", 8);
	if (rc) {
		cifssrv_debug("could not update with context\n");
		goto smb3signkey_ret;
	}

	rc = crypto_shash_update(&sess->server->secmech.sdeschmacsha256->shash,
			L, 4);
	if (rc) {
		cifssrv_debug("could not update with L\n");
		goto smb3signkey_ret;
	}

	rc = crypto_shash_final(&sess->server->secmech.sdeschmacsha256->shash,
			hashptr);
	if (rc) {
		cifssrv_debug("Could not generate hmacmd5 hash error %d\n", rc);
		goto smb3signkey_ret;
	}

	memcpy(key, hashptr, key_size);

smb3signkey_ret:
	return rc;
}

int calc_preauth_integrity_hash(struct tcp_server_info *server, int hash_id,
	char *buf, __u8 *pi_hash)
{
	int rc = -1;
	struct crypto_shash *c_shash;
	struct sdesc *c_sdesc;
	struct smb2_hdr *rcv_hdr2 = (struct smb2_hdr *)buf;
	char *all_bytes_msg = rcv_hdr2->ProtocolId;
	int msg_size = be32_to_cpu(rcv_hdr2->smb2_buf_length);

	if (hash_id == SMB2_PREAUTH_INTEGRITY_SHA512) {
		rc = crypto_sha512_alloc(server);
		if (rc) {
			cifssrv_debug("could not alloc sha512 rc %d\n", rc);
			goto out;
		}

		c_shash = server->secmech.sha512;
		c_sdesc = server->secmech.sdescsha512;
	} else
		goto out;

	rc = crypto_shash_init(&c_sdesc->shash);
	if (rc) {
		cifssrv_debug("could not init shashn");
		goto out;
	}

	rc = crypto_shash_update(&c_sdesc->shash,
				server->Preauth_HashValue, 64);
	if (rc) {
		cifssrv_debug("could not update with n\n");
		goto out;
	}

	rc = crypto_shash_update(&c_sdesc->shash, all_bytes_msg, msg_size);
	if (rc) {
		cifssrv_debug("could not update with n\n");
		goto out;
	}

	rc = crypto_shash_final(&c_sdesc->shash, pi_hash);
	if (rc) {
		cifssrv_debug("Could not generate hash err : %d\n", rc);
		goto out;
	}
out:
	return rc;
}
#endif
