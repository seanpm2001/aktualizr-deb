#include "crypto.h"

#include <array>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <sodium.h>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/scoped_array.hpp>

#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "openssl_compat.h"
#include "utilities/utils.h"

#if !AKTUALIZR_OPENSSL_PRE_3
#include <openssl/provider.h>
#endif

PublicKey::PublicKey(const boost::filesystem::path &path) : value_(Utils::readFile(path)) {
  type_ = Crypto::IdentifyRSAKeyType(value_);
}

PublicKey::PublicKey(const Json::Value &uptane_json) {
  std::string keytype;
  std::string keyvalue;

  try {
    if (!uptane_json["keytype"].isString()) {
      type_ = KeyType::kUnknown;
      return;
    }
    if (!uptane_json["keyval"].isObject()) {
      type_ = KeyType::kUnknown;
      return;
    }

    if (!uptane_json["keyval"]["public"].isString()) {
      type_ = KeyType::kUnknown;
      return;
    }

    keytype = uptane_json["keytype"].asString();
    keyvalue = uptane_json["keyval"]["public"].asString();
  } catch (const std::exception &ex) {
    LOG_ERROR << "Failed to initialize public key: " << ex.what();
    type_ = KeyType::kUnknown;
    return;
  }

  std::transform(keytype.begin(), keytype.end(), keytype.begin(), ::tolower);

  KeyType type;
  if (keytype == "ed25519") {
    type = KeyType::kED25519;
  } else if (keytype == "rsa") {
    type = Crypto::IdentifyRSAKeyType(keyvalue);
    if (type == KeyType::kUnknown) {
      LOG_WARNING << "Couldn't identify length of RSA key";
    }
  } else {
    type = KeyType::kUnknown;
  }
  type_ = type;
  value_ = keyvalue;
}

PublicKey::PublicKey(const std::string &value, KeyType type) : value_(value), type_(type) {
  if (Crypto::IsRsaKeyType(type)) {
    if (type != Crypto::IdentifyRSAKeyType(value)) {
      throw std::logic_error("RSA key length is incorrect");
    }
  }
}

bool PublicKey::VerifySignature(const std::string &signature, const std::string &message) const {
  switch (type_) {
    case KeyType::kED25519:
      return Crypto::ED25519Verify(boost::algorithm::unhex(value_), Utils::fromBase64(signature), message);
    case KeyType::kRSA2048:
    case KeyType::kRSA3072:
    case KeyType::kRSA4096:
      return Crypto::RSAPSSVerify(value_, Utils::fromBase64(signature), message);
    default:
      return false;
  }
}

bool PublicKey::operator==(const PublicKey &rhs) const { return value_ == rhs.value_ && type_ == rhs.type_; }
Json::Value PublicKey::ToUptane() const {
  Json::Value res;
  switch (type_) {
    case KeyType::kRSA2048:
    case KeyType::kRSA3072:
    case KeyType::kRSA4096:
      res["keytype"] = "RSA";
      break;
    case KeyType::kED25519:
      res["keytype"] = "ED25519";
      break;
    case KeyType::kUnknown:
      res["keytype"] = "unknown";
      break;
    default:
      throw std::range_error("Unknown key type in PublicKey::ToUptane");
  }
  res["keyval"]["public"] = value_;
  return res;
}

std::string PublicKey::KeyId() const {
  std::string key_content = value_;
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
  boost::algorithm::trim_right_if(key_content, boost::algorithm::is_any_of("\n"));
  std::string keyid = boost::algorithm::hex(Crypto::sha256digest(Utils::jsonToCanonicalStr(Json::Value(key_content))));
  std::transform(keyid.begin(), keyid.end(), keyid.begin(), ::tolower);
  return keyid;
}

std::string Crypto::sha256digest(const std::string &text) {
  std::array<unsigned char, crypto_hash_sha256_BYTES> sha256_hash{};
  crypto_hash_sha256(sha256_hash.data(), reinterpret_cast<const unsigned char *>(text.c_str()), text.size());
  return std::string(reinterpret_cast<char *>(sha256_hash.data()), crypto_hash_sha256_BYTES);
}

std::string Crypto::sha256digestHex(const std::string &text) {
  return boost::algorithm::to_lower_copy(boost::algorithm::hex(sha256digest(text)));
}

std::string Crypto::sha512digest(const std::string &text) {
  std::array<unsigned char, crypto_hash_sha512_BYTES> sha512_hash{};
  crypto_hash_sha512(sha512_hash.data(), reinterpret_cast<const unsigned char *>(text.c_str()), text.size());
  return std::string(reinterpret_cast<char *>(sha512_hash.data()), crypto_hash_sha512_BYTES);
}

std::string Crypto::sha512digestHex(const std::string &text) {
  return boost::algorithm::to_lower_copy(boost::algorithm::hex(sha512digest(text)));
}

std::string Crypto::RSAPSSSign(ENGINE *engine, const std::string &private_key, const std::string &message) {
  StructGuard<EVP_PKEY> key(nullptr, EVP_PKEY_free);
  StructGuard<RSA> rsa(nullptr, RSA_free);
  if (engine != nullptr) {
    // TODO(OTA-2138): this call leaks memory somehow...
    key.reset(ENGINE_load_private_key(engine, private_key.c_str(), nullptr, nullptr));

    if (key == nullptr) {
      LOG_ERROR << "ENGINE_load_private_key failed with error " << ERR_error_string(ERR_get_error(), nullptr);
      return std::string();
    }

    rsa.reset(EVP_PKEY_get1_RSA(key.get()));
    if (rsa == nullptr) {
      LOG_ERROR << "EVP_PKEY_get1_RSA failed with error " << ERR_error_string(ERR_get_error(), nullptr);
      return std::string();
    }
  } else {
    StructGuard<BIO> bio(BIO_new_mem_buf(const_cast<char *>(private_key.c_str()), static_cast<int>(private_key.size())),
                         BIO_vfree);
    key.reset(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
    if (key != nullptr) {
      rsa.reset(EVP_PKEY_get1_RSA(key.get()));
    }

    if (rsa == nullptr) {
      LOG_ERROR << "PEM_read_bio_PrivateKey failed with error " << ERR_error_string(ERR_get_error(), nullptr);
      return std::string();
    }

#if AKTUALIZR_OPENSSL_PRE_11
    RSA_set_method(rsa.get(), RSA_PKCS1_SSLeay());
#else
    RSA_set_method(rsa.get(), RSA_PKCS1_OpenSSL());
#endif
  }

  const auto sign_size = static_cast<unsigned int>(RSA_size(rsa.get()));
  boost::scoped_array<unsigned char> EM(new unsigned char[sign_size]);
  boost::scoped_array<unsigned char> pSignature(new unsigned char[sign_size]);

  std::string digest = Crypto::sha256digest(message);
  int status = RSA_padding_add_PKCS1_PSS(rsa.get(), EM.get(), reinterpret_cast<const unsigned char *>(digest.c_str()),
                                         EVP_sha256(), -1 /* maximum salt length*/);
  if (status == 0) {
    LOG_ERROR << "RSA_padding_add_PKCS1_PSS failed with error " << ERR_error_string(ERR_get_error(), nullptr);
    return std::string();
  }

  /* perform digital signature */
  status = RSA_private_encrypt(RSA_size(rsa.get()), EM.get(), pSignature.get(), rsa.get(), RSA_NO_PADDING);
  if (status == -1) {
    LOG_ERROR << "RSA_private_encrypt failed with error " << ERR_error_string(ERR_get_error(), nullptr);
    return std::string();
  }
  std::string retval = std::string(reinterpret_cast<char *>(pSignature.get()), sign_size);
  return retval;
}

std::string Crypto::Sign(KeyType key_type, ENGINE *engine, const std::string &private_key, const std::string &message) {
  if (key_type == KeyType::kED25519) {
    return Crypto::ED25519Sign(boost::algorithm::unhex(private_key), message);
  }
  return Crypto::RSAPSSSign(engine, private_key, message);
}

std::string Crypto::ED25519Sign(const std::string &private_key, const std::string &message) {
  std::array<unsigned char, crypto_sign_BYTES> sig{};
  crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char *>(message.c_str()), message.size(),
                       reinterpret_cast<const unsigned char *>(private_key.c_str()));
  return std::string(reinterpret_cast<char *>(sig.data()), crypto_sign_BYTES);
}

bool Crypto::RSAPSSVerify(const std::string &public_key, const std::string &signature, const std::string &message) {
  StructGuard<RSA> rsa(nullptr, RSA_free);
  StructGuard<BIO> bio(BIO_new_mem_buf(const_cast<char *>(public_key.c_str()), static_cast<int>(public_key.size())),
                       BIO_vfree);
  {
    RSA *r = nullptr;
    if (PEM_read_bio_RSA_PUBKEY(bio.get(), &r, nullptr, nullptr) == nullptr) {
      LOG_ERROR << "PEM_read_bio_RSA_PUBKEY failed with error " << ERR_error_string(ERR_get_error(), nullptr);
      return false;
    }
    rsa.reset(r);
  }

#if AKTUALIZR_OPENSSL_PRE_11
  RSA_set_method(rsa.get(), RSA_PKCS1_SSLeay());
#else
  RSA_set_method(rsa.get(), RSA_PKCS1_OpenSSL());
#endif

  const auto size = static_cast<unsigned int>(RSA_size(rsa.get()));
  boost::scoped_array<unsigned char> pDecrypted(new unsigned char[size]);
  /* now we will verify the signature
    Start by a RAW decrypt of the signature
  */
  int status =
      RSA_public_decrypt(static_cast<int>(signature.size()), reinterpret_cast<const unsigned char *>(signature.c_str()),
                         pDecrypted.get(), rsa.get(), RSA_NO_PADDING);
  if (status == -1) {
    LOG_ERROR << "RSA_public_decrypt failed with error " << ERR_error_string(ERR_get_error(), nullptr);
    return false;
  }

  std::string digest = Crypto::sha256digest(message);

  /* verify the data */
  status = RSA_verify_PKCS1_PSS(rsa.get(), reinterpret_cast<const unsigned char *>(digest.c_str()), EVP_sha256(),
                                pDecrypted.get(), -2 /* salt length recovered from signature*/);

  return status == 1;
}
bool Crypto::ED25519Verify(const std::string &public_key, const std::string &signature, const std::string &message) {
  if (public_key.size() < crypto_sign_PUBLICKEYBYTES || signature.size() < crypto_sign_BYTES) {
    return false;
  }
  return crypto_sign_verify_detached(reinterpret_cast<const unsigned char *>(signature.c_str()),
                                     reinterpret_cast<const unsigned char *>(message.c_str()), message.size(),
                                     reinterpret_cast<const unsigned char *>(public_key.c_str())) == 0;
}

bool Crypto::parseP12(BIO *p12_bio, const std::string &p12_password, std::string *out_pkey, std::string *out_cert,
                      std::string *out_ca) {
#if AKTUALIZR_OPENSSL_PRE_11
  SSLeay_add_all_algorithms();
#endif
  StructGuard<PKCS12> p12(d2i_PKCS12_bio(p12_bio, nullptr), PKCS12_free);
  if (p12 == nullptr) {
    LOG_ERROR << "Could not read from " << p12_bio << " file pointer";
    return false;
  }

  // use a lambda here because sk_X509_pop_free is a macro
  auto stackx509_free = [](STACK_OF(X509) * stack) { sk_X509_pop_free(stack, X509_free); };

  StructGuard<EVP_PKEY> pkey(nullptr, EVP_PKEY_free);
  StructGuard<X509> x509_cert(nullptr, X509_free);
  StructGuard<STACK_OF(X509)> ca_certs(nullptr, stackx509_free);
  {
    EVP_PKEY *pk;
    X509 *x509c = nullptr;
    STACK_OF(X509) *cacs = nullptr;
    if (PKCS12_parse(p12.get(), p12_password.c_str(), &pk, &x509c, &cacs) == 0) {
      LOG_ERROR << "Could not parse file from " << p12_bio << " source pointer";
      return false;
    }
    pkey.reset(pk);
    x509_cert.reset(x509c);
    ca_certs.reset(cacs);
  }

  StructGuard<BIO> pkey_pem_sink(BIO_new(BIO_s_mem()), BIO_vfree);
  if (pkey_pem_sink == nullptr) {
    LOG_ERROR << "Could not open pkey buffer for writing";
    return false;
  }
  PEM_write_bio_PrivateKey(pkey_pem_sink.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);

  char *pkey_buf;
  auto pkey_len = BIO_get_mem_data(pkey_pem_sink.get(), &pkey_buf);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
  *out_pkey = std::string(pkey_buf, static_cast<size_t>(pkey_len));

  char *cert_buf;
  size_t cert_len;
  StructGuard<BIO> cert_sink(BIO_new(BIO_s_mem()), BIO_vfree);
  if (cert_sink == nullptr) {
    LOG_ERROR << "Could not open certificate buffer for writing";
    return false;
  }
  PEM_write_bio_X509(cert_sink.get(), x509_cert.get());

  char *ca_buf;
  size_t ca_len;
  StructGuard<BIO> ca_sink(BIO_new(BIO_s_mem()), BIO_vfree);
  if (ca_sink == nullptr) {
    LOG_ERROR << "Could not open ca buffer for writing";
    return false;
  }
  X509 *ca_cert = nullptr;
  for (int i = 0; i < sk_X509_num(ca_certs.get()); i++) {
    ca_cert = sk_X509_value(ca_certs.get(), i);
    PEM_write_bio_X509(ca_sink.get(), ca_cert);
    PEM_write_bio_X509(cert_sink.get(), ca_cert);
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  ca_len = static_cast<size_t>(BIO_get_mem_data(ca_sink.get(), &ca_buf));
  *out_ca = std::string(ca_buf, ca_len);

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  cert_len = static_cast<size_t>(BIO_get_mem_data(cert_sink.get(), &cert_buf));
  *out_cert = std::string(cert_buf, cert_len);

  return true;
}

std::string Crypto::extractSubjectCN(const std::string &cert) {
  StructGuard<BIO> bio(BIO_new_mem_buf(const_cast<char *>(cert.c_str()), static_cast<int>(cert.size())), BIO_vfree);
  StructGuard<X509> x(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
  if (x == nullptr) {
    throw std::runtime_error("Could not parse certificate");
  }

  int len = X509_NAME_get_text_by_NID(X509_get_subject_name(x.get()), NID_commonName, nullptr, 0);
  if (len < 0) {
    throw std::runtime_error("Could not get CN from certificate");
  }
  boost::scoped_array<char> buf(new char[len + 1]);
  X509_NAME_get_text_by_NID(X509_get_subject_name(x.get()), NID_commonName, buf.get(), len + 1);
  return std::string(buf.get());
}

StructGuard<EVP_PKEY> Crypto::generateRSAKeyPairEVP(KeyType key_type) {
  int bits;
  switch (key_type) {
    case KeyType::kRSA2048:
      bits = 2048;
      break;
    case KeyType::kRSA3072:
      bits = 3072;
      break;
    case KeyType::kRSA4096:
      bits = 4096;
      break;
    default:
      return {nullptr, EVP_PKEY_free};
  }

  return Crypto::generateRSAKeyPairEVP(bits);
}

StructGuard<EVP_PKEY> Crypto::generateRSAKeyPairEVP(const int bits) {
  if (bits < 31) {  // sic!
    throw std::runtime_error("RSA key size can't be smaller than 31 bits");
  }

  int ret = RAND_status();
  if (ret != 1) { /* random generator has NOT been seeded with enough data */
    ret = RAND_poll();
    if (ret != 1) { /* seed data was NOT generated */
      throw std::runtime_error("Random generator has not been sufficiently seeded.");
    }
  }

  /* exponent - RSA_F4 is defined as 0x10001L */
  StructGuard<BIGNUM> bne(BN_new(), BN_free);
  if (BN_set_word(bne.get(), RSA_F4) != 1) {
    throw std::runtime_error(std::string("BN_set_word failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  StructGuard<RSA> rsa(RSA_new(), RSA_free);
  if (RSA_generate_key_ex(rsa.get(), bits, bne.get(), nullptr) != 1) {
    throw std::runtime_error(std::string("RSA_generate_key_ex failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  StructGuard<EVP_PKEY> pkey(EVP_PKEY_new(), EVP_PKEY_free);
  if (pkey.get() == nullptr) {
    throw std::runtime_error(std::string("EVP_PKEY_new failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  // release the rsa pointer here, pkey is the new owner
  if (!EVP_PKEY_assign_RSA(pkey.get(), rsa.release())) {  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
    throw std::runtime_error(std::string("EVP_PKEY_assign_RSA failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }
  return pkey;
}

/**
 * Generate a RSA keypair
 * @param key_type Algorithm used to generate the key
 * @param public_key Generated public part of key
 * @param private_key Generated private part of key
 * @return true if the keys are present at the end of this function (either they were created or existed already)
 *         false if key generation failed
 */
bool Crypto::generateRSAKeyPair(KeyType key_type, std::string *public_key, std::string *private_key) {
  int ret = 0;
  StructGuard<EVP_PKEY> pkey = generateRSAKeyPairEVP(key_type);
  if (pkey == nullptr) {
    return false;
  }

  char *pubkey_buf;
  StructGuard<BIO> pubkey_sink(BIO_new(BIO_s_mem()), BIO_vfree);
  if (pubkey_sink == nullptr) {
    return false;
  }
  ret = PEM_write_bio_PUBKEY(pubkey_sink.get(), pkey.get());
  if (ret != 1) {
    return false;
  }
  auto pubkey_len = BIO_get_mem_data(pubkey_sink.get(), &pubkey_buf);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
  *public_key = std::string(pubkey_buf, static_cast<size_t>(pubkey_len));

  char *privkey_buf;
  StructGuard<BIO> privkey_sink(BIO_new(BIO_s_mem()), BIO_vfree);
  if (privkey_sink == nullptr) {
    return false;
  }

  ret = PEM_write_bio_RSAPrivateKey(privkey_sink.get(), static_cast<RSA *>(EVP_PKEY_get0(pkey.get())), nullptr, nullptr,
                                    0, nullptr, nullptr);
  if (ret != 1) {
    return false;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  auto privkey_len = BIO_get_mem_data(privkey_sink.get(), &privkey_buf);
  *private_key = std::string(privkey_buf, static_cast<size_t>(privkey_len));
  return true;
}

bool Crypto::generateEDKeyPair(std::string *public_key, std::string *private_key) {
  std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> pk{};
  std::array<unsigned char, crypto_sign_SECRETKEYBYTES> sk{};
  crypto_sign_keypair(pk.data(), sk.data());
  *public_key = boost::algorithm::hex(std::string(reinterpret_cast<char *>(pk.data()), crypto_sign_PUBLICKEYBYTES));
  // std::transform(public_key->begin(), public_key->end(), public_key->begin(), ::tolower);
  *private_key = boost::algorithm::hex(std::string(reinterpret_cast<char *>(sk.data()), crypto_sign_SECRETKEYBYTES));
  // std::transform(private_key->begin(), private_key->end(), private_key->begin(), ::tolower);
  return true;
}

bool Crypto::generateKeyPair(KeyType key_type, std::string *public_key, std::string *private_key) {
  if (key_type == KeyType::kED25519) {
    return Crypto::generateEDKeyPair(public_key, private_key);
  }
  return Crypto::generateRSAKeyPair(key_type, public_key, private_key);
}

bool Crypto::IsRsaKeyType(KeyType type) {
  switch (type) {
    case KeyType::kRSA2048:
    case KeyType::kRSA3072:
    case KeyType::kRSA4096:
      return true;
    default:
      return false;
  }
}

KeyType Crypto::IdentifyRSAKeyType(const std::string &public_key_pem) {
  StructGuard<BIO> bufio(BIO_new_mem_buf(reinterpret_cast<const void *>(public_key_pem.c_str()),
                                         static_cast<int>(public_key_pem.length())),
                         BIO_vfree);
  if (bufio.get() == nullptr) {
    throw std::runtime_error("BIO_new_mem_buf failed");
  }
  StructGuard<::RSA> rsa(PEM_read_bio_RSA_PUBKEY(bufio.get(), nullptr, nullptr, nullptr), RSA_free);

  if (rsa.get() == nullptr) {
    return KeyType::kUnknown;
  }

  int key_length = RSA_size(rsa.get()) * 8;
  // It is not clear from the OpenSSL documentation if RSA_size returns
  // exactly 2048 or 4096, or if this can vary. For now we will assume that if
  // OpenSSL has been asked to generate a 'N bit' key, RSA_size() will return
  // exactly N
  switch (key_length) {
    case 2048:
      return KeyType::kRSA2048;
    case 3072:
      return KeyType::kRSA3072;
    case 4096:
      return KeyType::kRSA4096;
    default:
      LOG_WARNING << "Weird key length:" << key_length;
      return KeyType::kUnknown;
  }
}

StructGuard<X509> Crypto::generateCert(const int rsa_bits, const int cert_days, const std::string &cert_c,
                                       const std::string &cert_st, const std::string &cert_o,
                                       const std::string &cert_cn, bool self_sign) {
  // create certificate
  StructGuard<X509> certificate(X509_new(), X509_free);
  if (certificate.get() == nullptr) {
    throw std::runtime_error(std::string("X509_new failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  X509_set_version(certificate.get(), 2);  // X509v3

  {
    std::random_device urandom;
    std::uniform_int_distribution<> serial_dist(0, (1UL << 20) - 1);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial_dist(urandom));
  }

  // create and set certificate subject name
  StructGuard<X509_NAME> subj(X509_NAME_new(), X509_NAME_free);
  if (subj.get() == nullptr) {
    throw std::runtime_error(std::string("X509_NAME_new failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  if (!cert_c.empty()) {
    if (X509_NAME_add_entry_by_txt(subj.get(), "C", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>(cert_c.c_str()), -1, -1, 0) == 0) {
      throw std::runtime_error(std::string("X509_NAME_add_entry_by_txt failed: ") +
                               ERR_error_string(ERR_get_error(), nullptr));
    }
  }

  if (!cert_st.empty()) {
    if (X509_NAME_add_entry_by_txt(subj.get(), "ST", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>(cert_st.c_str()), -1, -1, 0) == 0) {
      throw std::runtime_error(std::string("X509_NAME_add_entry_by_txt failed: ") +
                               ERR_error_string(ERR_get_error(), nullptr));
    }
  }

  if (!cert_o.empty()) {
    if (X509_NAME_add_entry_by_txt(subj.get(), "O", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>(cert_o.c_str()), -1, -1, 0) == 0) {
      throw std::runtime_error(std::string("X509_NAME_add_entry_by_txt failed: ") +
                               ERR_error_string(ERR_get_error(), nullptr));
    }
  }

  assert(!cert_cn.empty());
  if (X509_NAME_add_entry_by_txt(subj.get(), "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char *>(cert_cn.c_str()), -1, -1, 0) == 0) {
    throw std::runtime_error(std::string("X509_NAME_add_entry_by_txt failed: ") +
                             ERR_error_string(ERR_get_error(), nullptr));
  }

  if (X509_set_subject_name(certificate.get(), subj.get()) == 0) {
    throw std::runtime_error(std::string("X509_set_subject_name failed: ") +
                             ERR_error_string(ERR_get_error(), nullptr));
  }

  // create and set key.
  StructGuard<EVP_PKEY> certificate_pkey(Crypto::generateRSAKeyPairEVP(rsa_bits));

  if (X509_set_pubkey(certificate.get(), certificate_pkey.get()) == 0) {
    throw std::runtime_error(std::string("X509_set_pubkey failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  //    set validity period
  if (X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) == nullptr) {
    throw std::runtime_error(std::string("X509_gmtime_adj failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  if (X509_gmtime_adj(X509_get_notAfter(certificate.get()), 60L * 60L * 24L * cert_days) == nullptr) {
    throw std::runtime_error(std::string("X509_gmtime_adj failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  // self-sign
  if (self_sign) {
    const EVP_MD *cert_digest = EVP_sha256();
    if (X509_sign(certificate.get(), certificate_pkey.get(), cert_digest) == 0) {
      throw std::runtime_error(std::string("X509_sign failed: ") + ERR_error_string(ERR_get_error(), nullptr));
    }
    LOG_INFO << "Successfully self-signed the generated certificate. This should not be used in production!";
  }

  return certificate;
}

void Crypto::signCert(const std::string &cacert_path, const std::string &capkey_path, X509 *const certificate) {
  // read CA certificate
  std::string cacert_contents = Utils::readFile(cacert_path);
  StructGuard<BIO> bio_in_cacert(BIO_new_mem_buf(cacert_contents.c_str(), static_cast<int>(cacert_contents.size())),
                                 BIO_free_all);
  StructGuard<X509> ca_certificate(PEM_read_bio_X509(bio_in_cacert.get(), nullptr, nullptr, nullptr), X509_free);
  if (ca_certificate.get() == nullptr) {
    throw std::runtime_error(std::string("Reading CA certificate failed: ") +
                             ERR_error_string(ERR_get_error(), nullptr));
  }

  // read CA private key
  std::string capkey_contents = Utils::readFile(capkey_path);
  StructGuard<BIO> bio_in_capkey(BIO_new_mem_buf(capkey_contents.c_str(), static_cast<int>(capkey_contents.size())),
                                 BIO_free_all);
  StructGuard<EVP_PKEY> ca_privkey(PEM_read_bio_PrivateKey(bio_in_capkey.get(), nullptr, nullptr, nullptr),
                                   EVP_PKEY_free);
  if (ca_privkey.get() == nullptr) {
    throw std::runtime_error(std::string("PEM_read_bio_PrivateKey failed: ") +
                             ERR_error_string(ERR_get_error(), nullptr));
  }

  // set issuer name
  X509_NAME *ca_subj = X509_get_subject_name(ca_certificate.get());
  if (ca_subj == nullptr) {
    throw std::runtime_error(std::string("X509_get_subject_name failed: ") +
                             ERR_error_string(ERR_get_error(), nullptr));
  }

  if (X509_set_issuer_name(certificate, ca_subj) == 0) {
    throw std::runtime_error(std::string("X509_set_issuer_name failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  // sign
  const EVP_MD *cert_digest = EVP_sha256();
  if (X509_sign(certificate, ca_privkey.get(), cert_digest) == 0) {
    throw std::runtime_error(std::string("X509_sign failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }
}

void Crypto::serializeCert(std::string *pkey, std::string *cert, X509 *const certificate) {
  // serialize private key
  char *privkey_buf;
  StructGuard<BIO> privkey_file(BIO_new(BIO_s_mem()), BIO_vfree);
  if (privkey_file == nullptr) {
    throw std::runtime_error(std::string("BIO_new failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  StructGuard<EVP_PKEY> certificate_pkey(X509_get_pubkey(certificate), EVP_PKEY_free);
  if (certificate_pkey == nullptr) {
    throw std::runtime_error(std::string("X509_get_pubkey failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  StructGuard<RSA> certificate_rsa(EVP_PKEY_get1_RSA(certificate_pkey.get()), RSA_free);
  if (certificate_rsa == nullptr) {
    throw std::runtime_error(std::string("EVP_PKEY_get1_RSA failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }

  int ret =
      PEM_write_bio_RSAPrivateKey(privkey_file.get(), certificate_rsa.get(), nullptr, nullptr, 0, nullptr, nullptr);
  if (ret == 0) {
    throw std::runtime_error(std::string("PEM_write_RSAPrivateKey failed: ") +
                             ERR_error_string(ERR_get_error(), nullptr));
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  auto privkey_len = BIO_get_mem_data(privkey_file.get(), &privkey_buf);
  *pkey = std::string(privkey_buf, static_cast<size_t>(privkey_len));

  // serialize certificate
  char *cert_buf;
  StructGuard<BIO> cert_file(BIO_new(BIO_s_mem()), BIO_vfree);
  if (cert_file == nullptr) {
    throw std::runtime_error(std::string("BIO_new failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }
  ret = PEM_write_bio_X509(cert_file.get(), certificate);
  if (ret == 0) {
    throw std::runtime_error(std::string("PEM_write_bio_X509 failed: ") + ERR_error_string(ERR_get_error(), nullptr));
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  auto cert_len = BIO_get_mem_data(cert_file.get(), &cert_buf);
  *cert = std::string(cert_buf, static_cast<size_t>(cert_len));
}

MultiPartHasher::Ptr MultiPartHasher::create(Hash::Type hash_type) {
  switch (hash_type) {
    case Hash::Type::kSha256: {
      return std::make_shared<MultiPartSHA256Hasher>();
    }
    case Hash::Type::kSha512: {
      return std::make_shared<MultiPartSHA512Hasher>();
    }
    default: {
      LOG_ERROR << "Unsupported type of hashing: " << Hash::TypeString(hash_type);
      return nullptr;
    }
  }
}

std::string MultiPartSHA512Hasher::getHexDigest() {
  std::array<unsigned char, crypto_hash_sha512_BYTES> sha512_hash{};
  crypto_hash_sha512_final(&state_, sha512_hash.data());
  return boost::algorithm::hex(std::string(reinterpret_cast<char *>(sha512_hash.data()), crypto_hash_sha512_BYTES));
}

std::string MultiPartSHA256Hasher::getHexDigest() {
  std::array<unsigned char, crypto_hash_sha256_BYTES> sha256_hash{};
  crypto_hash_sha256_final(&state_, sha256_hash.data());
  return boost::algorithm::hex(std::string(reinterpret_cast<char *>(sha256_hash.data()), crypto_hash_sha256_BYTES));
}

Hash Hash::generate(Type type, const std::string &data) {
  std::string hash;

  switch (type) {
    case Type::kSha256: {
      hash = boost::algorithm::hex(Crypto::sha256digest(data));
      break;
    }
    case Type::kSha512: {
      hash = boost::algorithm::hex(Crypto::sha512digest(data));
      break;
    }
    default: {
      throw std::invalid_argument("Unsupported hash type");
    }
  }

  return Hash(type, hash);
}

Hash Hash::generate(Type type, std::istream &source, ssize_t *nread) {
  std::unique_ptr<MultiPartHasher> hasher;

  switch (type) {
    case Type::kSha256: {
      hasher = std_::make_unique<MultiPartSHA256Hasher>();
      break;
    }
    case Type::kSha512: {
      hasher = std_::make_unique<MultiPartSHA512Hasher>();
      break;
    }
    default: {
      throw std::invalid_argument("Unsupported hash type");
    }
  }

  // Process file in blocks.
  using BufferType = std::array<uint8_t, 64 * 1024>;
  auto buffer = std_::make_unique<BufferType>();

  // Determine the digest of the whole stream.
  ssize_t count = 0;
  do {
    source.read(reinterpret_cast<char *>(buffer->data()), static_cast<std::streamsize>(buffer->size()));
    hasher->update(buffer->data(), static_cast<uint64_t>(source.gcount()));
    count += static_cast<ssize_t>(source.gcount());
  } while (source.gcount() > 0);

  if (nread != nullptr) {
    *nread = count;
  }

  return hasher->getHash();
}

Hash::Hash(const std::string &type, const std::string &hash) : hash_(boost::algorithm::to_upper_copy(hash)) {
  if (type == "sha512") {
    type_ = Hash::Type::kSha512;
  } else if (type == "sha256") {
    type_ = Hash::Type::kSha256;
  } else {
    type_ = Hash::Type::kUnknownAlgorithm;
  }
}

Hash::Hash(Type type, const std::string &hash) : type_(type), hash_(boost::algorithm::to_upper_copy(hash)) {}

bool Hash::operator==(const Hash &other) const { return type_ == other.type_ && hash_ == other.hash_; }

std::string Hash::TypeString(Type type) {
  switch (type) {
    case Type::kSha256:
      return "sha256";
    case Type::kSha512:
      return "sha512";
    default:
      return "unknown";
  }
}

std::string Hash::TypeString() const { return TypeString(type_); }

Hash::Type Hash::type() const { return type_; }

std::ostream &operator<<(std::ostream &os, const Hash &h) {
  os << "Hash: " << h.hash_;
  return os;
}

std::string Hash::shortTag(const std::vector<Hash> &hashes) {
  Hash::Type best = Type::kUnknownAlgorithm;
  std::string res = "(unknown)";
  for (const auto &i : hashes) {
    if (i.type_ < best) {
      res.assign(i.hash_, 0, 12);
      best = i.type_;
    }
  }
  boost::algorithm::to_lower(res);
  return res;
}

class CryptoOpenSSlInit {
 public:
  // NOLINTNEXTLINE(*-use-equals-default)
  CryptoOpenSSlInit() {
#if !AKTUALIZR_OPENSSL_PRE_3
    OSSL_PROVIDER *legacy = OSSL_PROVIDER_try_load(nullptr, "legacy", 1);
    if (legacy == nullptr) {
      std::cout << "Warning: could not load 'legacy' OpenSSL provider";
    }
    OSSL_PROVIDER *default_p = OSSL_PROVIDER_try_load(nullptr, "default", 1);
    if (default_p == nullptr) {
      std::cout << "Warning: could not load 'default' OpenSSL provider";
    }
#endif
  }
};

CryptoOpenSSlInit const CryptoIniter{};
