/*
 * Copyright 2026 Ethan O'Connor
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

package org.openorienteering.mapper;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.nio.charset.StandardCharsets;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** Android Keystore-backed storage for Map Hub bearer credentials. */
public final class SecureCredentialStore {
    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String KEY_ALIAS = "OpenOrienteeringMapper.MapHub.v1";
    private static final String PREFERENCES = "mapper_map_hub_credentials";

    private SecureCredentialStore() {}

    private static Context applicationContext(Context context) {
        return context.getApplicationContext();
    }

    private static SecretKey key() throws Exception {
        KeyStore store = KeyStore.getInstance(KEYSTORE);
        store.load(null);
        java.security.Key existing = store.getKey(KEY_ALIAS, null);
        if (existing instanceof SecretKey)
            return (SecretKey) existing;

        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE);
        generator.init(new KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .build());
        return generator.generateKey();
    }

    public static boolean write(Context context, String account, String secret) {
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, key());
            cipher.updateAAD(account.getBytes(StandardCharsets.UTF_8));
            byte[] ciphertext = cipher.doFinal(secret.getBytes(StandardCharsets.UTF_8));
            String value = Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP)
                    + ":" + Base64.encodeToString(ciphertext, Base64.NO_WRAP);
            return applicationContext(context).getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
                    .edit().putString(account, value).commit();
        } catch (Exception exception) {
            return false;
        }
    }

    public static String read(Context context, String account) {
        try {
            SharedPreferences preferences = applicationContext(context)
                    .getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
            String value = preferences.getString(account, "");
            if (value == null || value.isEmpty())
                return "";
            String[] parts = value.split(":", 2);
            if (parts.length != 2)
                return "";
            byte[] iv = Base64.decode(parts[0], Base64.NO_WRAP);
            byte[] ciphertext = Base64.decode(parts[1], Base64.NO_WRAP);
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(128, iv));
            cipher.updateAAD(account.getBytes(StandardCharsets.UTF_8));
            return new String(cipher.doFinal(ciphertext), StandardCharsets.UTF_8);
        } catch (Exception exception) {
            return "";
        }
    }

    public static boolean remove(Context context, String account) {
        return applicationContext(context).getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
                .edit().remove(account).commit();
    }
}
