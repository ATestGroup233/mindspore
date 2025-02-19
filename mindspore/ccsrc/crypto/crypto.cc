/**
 * Copyright 2021 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "crypto/crypto.h"

namespace mindspore {
namespace crypto {
int64_t Min(int64_t a, int64_t b) { return a < b ? a : b; }

void *intToByte(Byte *byte, const int32_t &n) {
  memset(byte, 0, sizeof(Byte) * 4);
  byte[0] = (Byte)(0xFF & n);
  byte[1] = (Byte)((0xFF00 & n) >> 8);
  byte[2] = (Byte)((0xFF0000 & n) >> 16);
  byte[3] = (Byte)((0xFF000000 & n) >> 24);
  return byte;
}

int32_t ByteToint(const Byte *byteArray) {
  int32_t res = byteArray[0] & 0xFF;
  res |= ((byteArray[1] << 8) & 0xFF00);
  res |= ((byteArray[2] << 16) & 0xFF0000);
  res += ((byteArray[3] << 24) & 0xFF000000);
  return res;
}

bool IsCipherFile(std::string file_path) {
  std::ifstream fid(file_path, std::ios::in | std::ios::binary);
  if (!fid) {
    return false;
  }
  char *int_buf = new char[4];
  fid.read(int_buf, sizeof(int32_t));
  fid.close();
  auto flag = ByteToint(reinterpret_cast<Byte *>(int_buf));
  delete[] int_buf;
  return flag == MAGIC_NUM;
}
#if defined(_WIN32)
Byte *Encrypt(int64_t *encrypt_len, Byte *plain_data, const int64_t plain_len, Byte *key, const int32_t key_len,
              const std::string &enc_mode) {
  MS_EXCEPTION(NotSupportError) << "Unsupported feature in Windows platform.";
}

Byte *Decrypt(int64_t *decrypt_len, const std::string &encrypt_data_path, Byte *key, const int32_t key_len,
              const std::string &dec_mode) {
  MS_EXCEPTION(NotSupportError) << "Unsupported feature in Windows platform.";
}
#else

bool ParseEncryptData(const Byte *encrypt_data, const int32_t encrypt_len, Byte **iv, int32_t *iv_len,
                      Byte **cipher_data, int32_t *cipher_len) {
  // encrypt_data is organized in order to iv_len, iv, cipher_len, cipher_data
  Byte buf[4];
  memcpy_s(buf, 4, encrypt_data, 4);
  *iv_len = ByteToint(buf);
  memcpy_s(buf, 4, encrypt_data + *iv_len + 4, 4);
  *cipher_len = ByteToint(buf);
  if (*iv_len <= 0 || *cipher_len <= 0 || *iv_len + *cipher_len + 8 != encrypt_len) {
    MS_LOG(ERROR) << "Failed to parse encrypt data.";
    return false;
  }
  *iv = new Byte[*iv_len];
  memcpy_s(*iv, *iv_len, encrypt_data + 4, *iv_len);
  *cipher_data = new Byte[*cipher_len];
  memcpy_s(*cipher_data, *cipher_len, encrypt_data + *iv_len + 8, *cipher_len);
  return true;
}

bool ParseMode(std::string mode, std::string *alg_mode, std::string *work_mode) {
  std::smatch results;
  std::regex re("([A-Z]{3})-([A-Z]{3})");
  if (!std::regex_match(mode.c_str(), re)) {
    MS_LOG(ERROR) << "Mode " << mode << " is invalid.";
    return false;
  }
  std::regex_search(mode, results, re);
  *alg_mode = results[1];
  *work_mode = results[2];
  return true;
}

EVP_CIPHER_CTX *GetEVP_CIPHER_CTX(const std::string &work_mode, const Byte *key, const int32_t key_len, const Byte *iv,
                                  int flag) {
  int ret = 0;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (work_mode != "GCM" && work_mode != "CBC") {
    MS_LOG(ERROR) << "Work mode " << work_mode << " is invalid.";
    return nullptr;
  }

  const EVP_CIPHER *(*funcPtr)() = nullptr;
  if (work_mode == "GCM") {
    switch (key_len) {
      case 16:
        funcPtr = EVP_aes_128_gcm;
        break;
      case 24:
        funcPtr = EVP_aes_192_gcm;
        break;
      case 32:
        funcPtr = EVP_aes_256_gcm;
        break;
      default:
        MS_EXCEPTION(ValueError) << "The key length must be 16, 24 or 32, but got key length is " << key_len << ".";
    }
  } else if (work_mode == "CBC") {
    switch (key_len) {
      case 16:
        funcPtr = EVP_aes_128_cbc;
        break;
      case 24:
        funcPtr = EVP_aes_192_cbc;
        break;
      case 32:
        funcPtr = EVP_aes_256_cbc;
        break;
      default:
        MS_EXCEPTION(ValueError) << "The key length must be 16, 24 or 32, but got key length is " << key_len << ".";
    }
  }

  if (flag == 0) {
    ret = EVP_EncryptInit_ex(ctx, funcPtr(), NULL, key, iv);
  } else if (flag == 1) {
    ret = EVP_DecryptInit_ex(ctx, funcPtr(), NULL, key, iv);
  }

  if (ret != 1) {
    MS_LOG(ERROR) << "EVP_EncryptInit_ex failed";
    return nullptr;
  }
  if (work_mode == "CBC") EVP_CIPHER_CTX_set_padding(ctx, 1);
  return ctx;
}

bool _BlockEncrypt(Byte *encrypt_data, const int64_t encrypt_data_buf_len, int64_t *encrypt_data_len, Byte *plain_data,
                   const int64_t plain_len, Byte *key, const int32_t key_len, const std::string &enc_mode) {
  int32_t cipher_len = 0;

  int32_t iv_len = AES_BLOCK_SIZE;
  Byte *iv = new Byte[iv_len];
  RAND_bytes(iv, sizeof(Byte) * iv_len);

  Byte *iv_cpy = new Byte[16];
  memcpy_s(iv_cpy, 16, iv, 16);

  int32_t ret = 0;
  int32_t flen = 0;
  std::string alg_mode;
  std::string work_mode;
  if (!ParseMode(enc_mode, &alg_mode, &work_mode)) {
    return false;
  }

  auto ctx = GetEVP_CIPHER_CTX(work_mode, key, key_len, iv, 0);
  if (ctx == nullptr) {
    MS_LOG(ERROR) << "Failed to get EVP_CIPHER_CTX.";
    return false;
  }

  Byte *cipher_data;
  cipher_data = new Byte[plain_len + 16];
  ret = EVP_EncryptUpdate(ctx, cipher_data, &cipher_len, plain_data, plain_len);
  if (ret != 1) {
    MS_LOG(ERROR) << "EVP_EncryptUpdate failed";
    delete[] cipher_data;
    return false;
  }
  if (work_mode == "CBC") {
    EVP_EncryptFinal_ex(ctx, cipher_data + cipher_len, &flen);
    cipher_len += flen;
  }
  EVP_CIPHER_CTX_free(ctx);

  int64_t cur = 0;
  *encrypt_data_len = sizeof(int32_t) * 2 + iv_len + cipher_len;

  Byte *byte_buf = new Byte[4];
  intToByte(byte_buf, *encrypt_data_len);
  memcpy_s(encrypt_data + cur, encrypt_data_buf_len - cur, byte_buf, 4);
  cur += 4;
  intToByte(byte_buf, iv_len);
  memcpy_s(encrypt_data + cur, encrypt_data_buf_len - cur, byte_buf, 4);
  cur += 4;
  memcpy_s(encrypt_data + cur, encrypt_data_buf_len - cur, iv_cpy, iv_len);
  cur += iv_len;
  intToByte(byte_buf, cipher_len);
  memcpy_s(encrypt_data + cur, encrypt_data_buf_len - cur, byte_buf, 4);
  cur += 4;
  memcpy_s(encrypt_data + cur, encrypt_data_buf_len - cur, cipher_data, cipher_len);
  *encrypt_data_len += 4;

  delete[] byte_buf;
  delete[] cipher_data;
  return true;
}

bool _BlockDecrypt(Byte *plain_data, int32_t *plain_len, Byte *encrypt_data, const int64_t encrypt_len, Byte *key,
                   const int32_t key_len, const std::string &dec_mode) {
  std::string alg_mode;
  std::string work_mode;

  if (!ParseMode(dec_mode, &alg_mode, &work_mode)) {
    return false;
  }

  int32_t iv_len = 0;
  int32_t cipher_len = 0;
  Byte *iv = NULL;
  Byte *cipher_data = NULL;

  if (!ParseEncryptData(encrypt_data, encrypt_len, &iv, &iv_len, &cipher_data, &cipher_len)) {
    return false;
  }

  int ret = 0;
  int mlen = 0;

  auto ctx = GetEVP_CIPHER_CTX(work_mode, key, key_len, iv, 1);
  if (ctx == nullptr) {
    MS_LOG(ERROR) << "Failed to get EVP_CIPHER_CTX.";
    return false;
  }
  ret = EVP_DecryptUpdate(ctx, plain_data, plain_len, cipher_data, cipher_len);
  if (ret != 1) {
    MS_LOG(ERROR) << "EVP_DecryptUpdate failed";
    return false;
  }
  if (work_mode == "CBC") {
    ret = EVP_DecryptFinal_ex(ctx, plain_data + *plain_len, &mlen);
    if (ret != 1) {
      MS_LOG(ERROR) << "EVP_DecryptFinal_ex failed";
      return false;
    }
    *plain_len += mlen;
  }
  delete[] iv;
  delete[] cipher_data;
  EVP_CIPHER_CTX_free(ctx);
  return true;
}

Byte *Encrypt(int64_t *encrypt_len, Byte *plain_data, const int64_t plain_len, Byte *key, const int32_t key_len,
              const std::string &enc_mode) {
  int64_t cur_pos = 0;
  int64_t block_enc_len = 0;
  int64_t encrypt_buf_len = plain_len + (plain_len / MAX_BLOCK_SIZE + 1) * 100;
  int64_t block_enc_buf_len = MAX_BLOCK_SIZE + 100;
  Byte *byte_buf = new Byte[4];
  Byte *encrypt_data = new Byte[encrypt_buf_len];
  Byte *block_buf = new Byte[MAX_BLOCK_SIZE];
  Byte *block_enc_buf = new Byte[block_enc_buf_len];

  *encrypt_len = 0;
  while (cur_pos < plain_len) {
    int64_t cur_block_size = Min(MAX_BLOCK_SIZE, plain_len - cur_pos);
    memcpy_s(block_buf, MAX_BLOCK_SIZE, plain_data + cur_pos, cur_block_size);

    if (!_BlockEncrypt(block_enc_buf, block_enc_buf_len, &block_enc_len, block_buf, cur_block_size, key, key_len,
                       enc_mode)) {
      delete[] byte_buf;
      delete[] block_buf;
      delete[] block_enc_buf;
      delete[] encrypt_data;
      MS_EXCEPTION(ValueError) << "Failed to encrypt data, please check if enc_key or enc_mode is valid.";
    }
    intToByte(byte_buf, MAGIC_NUM);
    memcpy_s(encrypt_data + *encrypt_len, encrypt_buf_len - *encrypt_len, byte_buf, sizeof(int32_t));
    *encrypt_len += sizeof(int32_t);
    memcpy_s(encrypt_data + *encrypt_len, encrypt_buf_len - *encrypt_len, block_enc_buf, block_enc_len);
    *encrypt_len += block_enc_len;
    cur_pos += cur_block_size;
  }
  delete[] byte_buf;
  delete[] block_buf;
  delete[] block_enc_buf;
  return encrypt_data;
}

Byte *Decrypt(int64_t *decrypt_len, const std::string &encrypt_data_path, Byte *key, const int32_t key_len,
              const std::string &dec_mode) {
  char *block_buf = new char[MAX_BLOCK_SIZE * 2];
  char *int_buf = new char[4];
  Byte *decrypt_block_buf = new Byte[MAX_BLOCK_SIZE * 2];
  int32_t decrypt_block_len;

  std::ifstream fid(encrypt_data_path, std::ios::in | std::ios::binary);
  if (!fid) {
    MS_EXCEPTION(ValueError) << "Open file '" << encrypt_data_path << "' failed, please check the correct of the file.";
  }
  fid.seekg(0, std::ios_base::end);
  int64_t file_size = fid.tellg();
  fid.clear();
  fid.seekg(0);
  Byte *decrypt_data = new Byte[file_size];

  *decrypt_len = 0;
  while (fid.tellg() < file_size) {
    fid.read(int_buf, sizeof(int32_t));
    int cipher_flag = ByteToint(reinterpret_cast<Byte *>(int_buf));
    if (cipher_flag != MAGIC_NUM) {
      delete[] block_buf;
      delete[] int_buf;
      delete[] decrypt_block_buf;
      delete[] decrypt_data;
      MS_EXCEPTION(ValueError) << "File \"" << encrypt_data_path
                               << "\"is not an encrypted file and cannot be decrypted";
    }
    fid.read(int_buf, sizeof(int32_t));

    int32_t block_size = ByteToint(reinterpret_cast<Byte *>(int_buf));
    fid.read(block_buf, sizeof(char) * block_size);
    if (!(_BlockDecrypt(decrypt_block_buf, &decrypt_block_len, reinterpret_cast<Byte *>(block_buf), block_size, key,
                        key_len, dec_mode))) {
      delete[] block_buf;
      delete[] int_buf;
      delete[] decrypt_block_buf;
      delete[] decrypt_data;
      MS_EXCEPTION(ValueError) << "Failed to decrypt data, please check if dec_key or dec_mode is valid";
    }
    auto destMax = Min(file_size - *decrypt_len, SECUREC_MEM_MAX_LEN);
    memcpy_s(decrypt_data + *decrypt_len, destMax, decrypt_block_buf, decrypt_block_len);
    *decrypt_len += decrypt_block_len;
  }
  fid.close();
  delete[] block_buf;
  delete[] int_buf;
  delete[] decrypt_block_buf;
  return decrypt_data;
}
#endif
}  // namespace crypto
}  // namespace mindspore
