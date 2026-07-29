
#include <Arduino.h>
#include <MD5Builder.h>

#include "mykeyboard.h"
#include "passwords.h"
#include "sd_functions.h"
#include "type_convertion.h"
#include <globals.h>

String xorEncryptDecryptMD5(const String &input, const String &password, const int MD5_PASSES) {

    MD5Builder md5;
    String hash = password;

    for (int i = 0; i < MD5_PASSES; i++) {
        md5.begin();
        md5.add(hash);
        md5.calculate();
    }

    uint8_t md5Hash[16];
    md5.getBytes(md5Hash); // Store MD5 hash in the output array

    String output = input; // Copy input to output for modification
    for (size_t i = 0; i < input.length(); i++) {
        output[i] = input[i] ^ md5Hash[i % 16]; // XOR each byte with the MD5 hash
    }

    return output;
}

bool isValidAscii(const String &text) {
    for (int i = 0; i < text.length(); i++) {
        char c = text[i];
        // Check if the character is within the printable ASCII range or is a newline/carriage return
        if (!(c >= 32 && c <= 126) && c != 10 && c != 13) {
            return false; // Invalid character found
        }
    }
    return true; // All characters are valid
}

/* OLD:
String readDecryptedFileOLD(FS &fs, String filepath) {
  String cyphertext = readSmallFile(fs, filepath);
  if(cyphertext.length() == 0) return "";

  if(cachedPassword.length()==0) {
    cachedPassword = keyboard("", 32, "Password:", true);
    if(cachedPassword.length()==0) return "";  // cancelled
  }

  //Serial.println(cyphertext);
  //Serial.println(cachedPassword);

  // else try to decrypt
  String plaintext = decryptString(cyphertext, cachedPassword);

  // check if really plaintext
  if(!isValidAscii(plaintext)) {
    // invalidate cached password -> will ask again on the next try
    cachedPassword = "";
    Serial.println("invalid password");
    //Serial.println(plaintext);
    return "";
  }

  // else
  return plaintext;
}
*/

String readDecryptedFile(FS &fs, String filepath) {

    if (cachedPassword.length() == 0) {
        cachedPassword = keyboard("", 32, "Password:", true);
        if (cachedPassword.length() == 0 || cachedPassword == "\x1B") return ""; // cancelled
    }

    File cyphertextFile = fs.open(filepath, FILE_READ);
    if (!cyphertextFile) return "";

    String line;
    String cypertextData = "";
    String plaintext = "";
    bool unsupported_params = false;

    while (cyphertextFile.available()) {
        line = cyphertextFile.readStringUntil('\n');
        if (line.startsWith("Filetype:") && !line.endsWith("Bruce Encrypted File")) unsupported_params = true;
        if (line.startsWith("Algo:") && !line.endsWith("XOR")) unsupported_params = true;
        if (line.startsWith("KeyDerivationAlgo:") && !line.endsWith("MD5")) unsupported_params = true;
        if (line.startsWith("KeyDerivationPasses:") && !line.endsWith("10"))
            unsupported_params = true; // TODO: parse
        if (line.startsWith("Data:")) cypertextData = line.substring(strlen("Data:"));
    }

    cyphertextFile.close();

    if (unsupported_params || cypertextData.length() == 0) {
        Serial.println("err: invalid Encrypted file (altered?)");
        return "";
    }

    // else try decrypting
    cypertextData.trim();
    String cypertextDataDec = "";
    cypertextDataDec.reserve(cypertextData.length());

    // Tokenise on whitespace rather than stepping a fixed 3 characters. The
    // writer below used to emit a single character for values under 0x10, so
    // byte 0x08 was written "8" and every byte after it decoded one character
    // out of phase — decryption then failed with an empty reply that looked
    // exactly like a wrong password. Reading token by token recovers both the
    // zero-padded form written now and every unpadded file already on disk.
    for (int i = 0; i < (int)cypertextData.length();) {
        while (i < (int)cypertextData.length() && isspace((unsigned char)cypertextData[i])) i++;
        if (i >= (int)cypertextData.length()) break;

        uint8_t decimal = hexCharToDecimal(cypertextData[i++]);
        if (i < (int)cypertextData.length() && !isspace((unsigned char)cypertextData[i])) {
            decimal = (decimal << 4) | hexCharToDecimal(cypertextData[i++]);
        }
        cypertextDataDec += (char)decimal;
    }

    // Serial.println(cachedPassword);
    // Serial.println(cypertextData);
    // Serial.println(cypertextDataDec);

    plaintext = xorEncryptDecryptMD5(cypertextDataDec, cachedPassword, 10);

    if (!isValidAscii(plaintext)) {
        // invalidate cached password -> will ask again on the next try
        cachedPassword = "";
        displayError("decryption failed (invalid password?)");
        // Serial.println(plaintext);
        return "";
    }
    // else
    return (plaintext);
}

// void writeEncryptedFile(FS &fs, String filepath, String& plaintext)

String encryptString(String &plaintext, const String &password_str) {
    String dataStr = xorEncryptDecryptMD5(plaintext, password_str, 10);
    String dataStrHex = "";

    // Zero-pad to two characters. Without this, values under 0x10 emit a single
    // character and desynchronise a fixed-stride reader (see readDecryptedFile).
    // The uint8_t cast also stops a char over 0x7F sign-extending into a
    // multi-character hex string.
    for (size_t i = 0; i < dataStr.length(); i++) {
        uint8_t byte = (uint8_t)dataStr[i];
        if (byte < 0x10) dataStrHex += "0";
        dataStrHex += String(byte, HEX) + " ";
    }
    dataStrHex.toUpperCase();
    dataStrHex.trim();

    String out = "Filetype: Bruce Encrypted File\nVersion: 1\n";
    out += "Algo: XOR\n"; // TODO: add AES
    out += "KeyDerivationAlgo: MD5\n";
    out += "KeyDerivationPasses: 10\n";
    out += "Data: " + dataStrHex + "\n";

    return out;
}

/* OLD:
String decryptString(String& cypertext, const String& password_str)

  return xorEncryptDecryptMD5(cypertextData, password_str);
}
*/
