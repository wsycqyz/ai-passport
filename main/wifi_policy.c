// main/wifi_policy.c
#include "wifi_policy.h"

wifi_fail_t wifi_policy_classify(uint16_t reason)
{
    switch (reason) {
    case 200:   // BEACON_TIMEOUT
    case 201:   // NO_AP_FOUND
    case 212:   // NO_AP_FOUND_IN_RSSI_THRESHOLD
        return WIFI_FAIL_NOT_FOUND;
    case 14:    // MIC_FAILURE
    case 15:    // 4WAY_HANDSHAKE_TIMEOUT
    case 202:   // AUTH_FAIL
    case 204:   // HANDSHAKE_TIMEOUT
        return WIFI_FAIL_WRONG_PASSWORD;
    case 18:    // GROUP_CIPHER_INVALID
    case 19:    // PAIRWISE_CIPHER_INVALID
    case 20:    // AKMP_INVALID
    case 21:    // UNSUPP_RSN_IE_VERSION
    case 22:    // INVALID_RSN_IE_CAP
    case 23:    // 802_1X_AUTH_FAILED
    case 24:    // CIPHER_SUITE_REJECTED
    case 29:    // BAD_CIPHER_OR_AKM
    case 210:   // NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211:   // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
        return WIFI_FAIL_SECURITY;
    case 5:     // ASSOC_TOOMANY
        return WIFI_FAIL_AP_FULL;
    case 30:    // NOT_AUTHORIZED_THIS_LOCATION
    case 203:   // ASSOC_FAIL
    case 205:   // CONNECTION_FAIL
    case 208:   // ASSOC_COMEBACK_TIME_TOO_LONG
        return WIFI_FAIL_REJECTED;
    default:
        return WIFI_FAIL_UNKNOWN;
    }
}

uint8_t wifi_policy_attempts(wifi_fail_t fail)
{
    switch (fail) {
    case WIFI_FAIL_SECURITY:
    case WIFI_FAIL_BAD_PASSWORD:
    case WIFI_FAIL_DRIVER:
        return 1;   // retrying cannot help
    case WIFI_FAIL_NOT_FOUND:
    case WIFI_FAIL_WRONG_PASSWORD:
        return 2;   // one retry rides out a missed scan or a lost handshake frame
    default:
        return 3;
    }
}

const char *wifi_policy_title(wifi_fail_t fail)
{
    switch (fail) {
    case WIFI_FAIL_NOT_FOUND:      return "Network not found";
    case WIFI_FAIL_WRONG_PASSWORD: return "Wrong password";
    case WIFI_FAIL_SECURITY:       return "Security not supported";
    case WIFI_FAIL_AP_FULL:        return "Router is full";
    case WIFI_FAIL_REJECTED:       return "Router refused";
    case WIFI_FAIL_NO_IP:          return "No IP address";
    case WIFI_FAIL_TIMEOUT:        return "Connection timed out";
    case WIFI_FAIL_BAD_PASSWORD:   return "Invalid password";
    case WIFI_FAIL_DRIVER:         return "Wi-Fi error";
    case WIFI_FAIL_NONE:
    case WIFI_FAIL_UNKNOWN:
    default:                       return "Error";
    }
}

const char *wifi_policy_hint(wifi_fail_t fail)
{
    switch (fail) {
    case WIFI_FAIL_NOT_FOUND:
        return "Check the Wi-Fi name, move closer to the router, and use a 2.4 GHz network.";
    case WIFI_FAIL_WRONG_PASSWORD:
        return "The router rejected the password. Check it and send the sound again.";
    case WIFI_FAIL_SECURITY:
        return "Use WPA, WPA2 or WPA3 Personal. Enterprise networks are not supported.";
    case WIFI_FAIL_AP_FULL:
        return "Too many devices are connected to this router.";
    case WIFI_FAIL_REJECTED:
        return "The router refused the connection. Check MAC filtering or restart it.";
    case WIFI_FAIL_NO_IP:
        return "Joined the network, but the router assigned no address (DHCP).";
    case WIFI_FAIL_TIMEOUT:
        return "The router did not answer in time. Move closer and try again.";
    case WIFI_FAIL_BAD_PASSWORD:
        return "Use 8-63 characters or 64 hex digits.";
    case WIFI_FAIL_DRIVER:
        return "The Wi-Fi radio did not start. Restart the device.";
    case WIFI_FAIL_NONE:
    case WIFI_FAIL_UNKNOWN:
    default:
        return "The connection failed for an unknown reason.";
    }
}
