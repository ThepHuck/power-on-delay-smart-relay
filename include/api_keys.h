#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "api_key_policy.h"

void loadApiKeys();
void updateApiKeys();
uint64_t apiSecurityTime();
int authenticateApiKey(const String& authorization);
int createApiKey(const String& label, ApiKeyExpiration mode, unsigned int days,
                 const String& date, String& token, String& id, String& error);
int revokeApiKey(const String& id, String& error);
void apiKeysJson(JsonArray output);
String apiKeysConfigHTML(const String& newToken = "");
void handleCreateApiKey();
void handleRevokeApiKey();
