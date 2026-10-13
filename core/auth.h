#pragma once
#include <stdbool.h>
bool tg_password_hash(const char *password, const unsigned char *salt, unsigned char *out);
