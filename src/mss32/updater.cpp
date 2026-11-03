#include "updater.h"

#include <windows.h>
#include <wininet.h>
#include <openssl/evp.h>
#include <string>

#include "shared.h"
#include "system.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_net.h"
#include "../shared/cod2_cmd.h"
#include "../shared/cod2_shared.h"
#include "../shared/updater_https.h"


#define cl_updateAvailable (*(dvar_t **)(0x0096b644))
#define cl_updateVersion (*(dvar_t **)(0x0096b640))
#define cl_updateOldVersion (*(dvar_t **)(0x0096b64c))
#define cl_updateFiles (*(dvar_t **)(0x0096b5d4))

#define clientState                   (*((clientState_e*)0x00609fe0))
#define demo_isPlaying                (*((int*)0x0064a170))

int                 updater_lastClientState = -1;
int                 updater_lastMainMenuClosed = 0;
bool                updater_waitingForResponse = false;
int                 updater_waitingForResponseRepeats = 0;
DWORD               updater_waitingForResponseTime = 0;
bool                updater_forcedUpdate = false;
std::string         updater_expectedSha256; // SHA-256 of the file in cl_updateFiles, from update.json
dvar_t*             sv_update;


void updater_showForceUpdateDialog() {
    Com_Error(ERR_DROP, "Update required\n\nA new version of the CoD2x 125 Hz build must be installed.\nPlease update to version %s.\n", cl_updateVersion->value.string);
}

// Lowercase hex SHA-256 of a file
static bool updater_fileSha256(const char* path, std::string& out) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);

    char buffer[4096];
    DWORD bytesRead;
    while (ok && ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0)
        ok = EVP_DigestUpdate(ctx, buffer, bytesRead);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    ok = ok && EVP_DigestFinal_ex(ctx, digest, &digestLen);
    EVP_MD_CTX_free(ctx);
    CloseHandle(hFile);

    if (!ok)
        return false;
    out.clear();
    char hex[3];
    for (unsigned int i = 0; i < digestLen; i++) {
        snprintf(hex, sizeof(hex), "%02x", digest[i]);
        out += hex;
    }
    return true;
}

bool updater_downloadDLL(const char *url, const char *downloadPath, char *errorBuffer, size_t errorBufferSize, int retryCount = 0) {
    // Initialize WinINet
    HINTERNET hInternet = InternetOpenA("CoD2x " APP_VERSION " Update Downloader", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        snprintf(errorBuffer, errorBufferSize, "Failed to initialize WinINet.");
        return false;
    }

    // Open URL
    HINTERNET hFile = InternetOpenUrlA(hInternet, url, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hFile) {
        snprintf(errorBuffer, errorBufferSize, "Failed to open URL '%s'.", url);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Check HTTP status code
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!HttpQueryInfoA(hFile, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusCodeSize, NULL)) {
        snprintf(errorBuffer, errorBufferSize, "Failed to query HTTP status code for URL '%s'.", url);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Handle HTTP errors (e.g., 404 Not Found)
    if (statusCode != 200) {
        snprintf(errorBuffer, errorBufferSize, "HTTP error %lu encountered for URL '%s'.", statusCode, url);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Retrieve expected file size from the Content-Length header.
    DWORD contentLength = 0;
    DWORD contentLengthSize = sizeof(contentLength);
    if (!HttpQueryInfoA(hFile, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &contentLength, &contentLengthSize, NULL)) {
        snprintf(errorBuffer, errorBufferSize, "Failed to retrieve content length for URL '%s'.", url);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Create file using CreateFile
    HANDLE hLocalFile = CreateFileA(
        downloadPath,
        GENERIC_WRITE,           // Write access
        FILE_SHARE_READ,         // Allow read sharing
        NULL,                    // Default security
        CREATE_ALWAYS,           // Create new file or overwrite
        FILE_ATTRIBUTE_NORMAL,   // Normal file
        NULL                     // No template
    );

    if (hLocalFile == INVALID_HANDLE_VALUE) {
        snprintf(errorBuffer, errorBufferSize, "Failed to create file at '%s'.", downloadPath);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);
        return false;
    }

    // Download and write data
    char buffer[4096];
    DWORD bytesRead;
    DWORD bytesWritten;
    DWORD totalBytesDownloaded = 0;

    while (InternetReadFile(hFile, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        totalBytesDownloaded += bytesRead;
        if (!WriteFile(hLocalFile, buffer, bytesRead, &bytesWritten, NULL)) {
            snprintf(errorBuffer, errorBufferSize, "Failed to write to file '%s'.", downloadPath);
            CloseHandle(hLocalFile);
            InternetCloseHandle(hFile);
            InternetCloseHandle(hInternet);
            return false;
        }

        if (bytesWritten != bytesRead) {
            snprintf(errorBuffer, errorBufferSize, "Mismatch in bytes read and written to file '%s'.", downloadPath);
            CloseHandle(hLocalFile);
            InternetCloseHandle(hFile);
            InternetCloseHandle(hInternet);
            return false;
        }
    }

    // Validate the download integrity
    if (totalBytesDownloaded != contentLength) {
        CloseHandle(hLocalFile);
        InternetCloseHandle(hFile);
        InternetCloseHandle(hInternet);

        // Try again
        retryCount++;

        if (retryCount > 3) {
            snprintf(errorBuffer, errorBufferSize, "File download incomplete: expected %lu bytes, got %lu bytes.", contentLength, totalBytesDownloaded);
            return false; // Too many retries
        }

        return updater_downloadDLL(url, downloadPath, errorBuffer, errorBufferSize, retryCount);
    }

    // Clean up
    CloseHandle(hLocalFile);
    InternetCloseHandle(hFile);
    InternetCloseHandle(hInternet);

    return true; // Success
}


bool updater_downloadAndReplaceDllFile(const char *url, const char *sha256, char *errorBuffer, size_t errorBufferSize) {

    const char destinationFileName[] = "mss32_new.dll";
    const char oldFileName[] = "mss32_old.dll";
    const char currentFileName[] = "mss32.dll";

    char dllFilePathNew[sizeof(EXE_DIRECTORY_PATH) + sizeof(destinationFileName) + 1];
    char dllFilePathOld[sizeof(EXE_DIRECTORY_PATH) + sizeof(oldFileName) + 1];
    char dllFilePathCurrent[sizeof(EXE_DIRECTORY_PATH) + sizeof(currentFileName) + 1];

    // Construct the destination path (same directory as the executable)
    snprintf(dllFilePathNew, sizeof(dllFilePathNew), "%s\\%s", EXE_DIRECTORY_PATH, destinationFileName);  
    snprintf(dllFilePathOld, sizeof(dllFilePathOld), "%s\\%s", EXE_DIRECTORY_PATH, oldFileName);
    snprintf(dllFilePathCurrent, sizeof(dllFilePathCurrent), "%s\\%s", EXE_DIRECTORY_PATH, currentFileName);


    // Download the DLL
    bool ok = updater_downloadDLL(url, dllFilePathNew, errorBuffer, errorBufferSize);
    if (!ok) return false;

    // 125 Hz fork: install only the exact file listed in update.json
    std::string actualSha256;
    if (!updater_fileSha256(dllFilePathNew, actualSha256) || actualSha256 != sha256) {
        snprintf(errorBuffer, errorBufferSize, "Downloaded file does not match the expected SHA-256.");
        DeleteFileA(dllFilePathNew);
        return false;
    }


    // Rename the existing DLL
    if (!MoveFileEx(dllFilePathCurrent, dllFilePathOld, MOVEFILE_REPLACE_EXISTING)) {
        snprintf(errorBuffer, errorBufferSize, "Error renaming DLL from '%s' to '%s'.", dllFilePathCurrent, dllFilePathOld);

        return false;
    }

    // Rename the new DLL to the original name
    if (!MoveFileEx(dllFilePathNew, dllFilePathCurrent, FALSE)) {
        snprintf(errorBuffer, errorBufferSize, "Error copying new DLL from '%s' to '%s'.", dllFilePathNew, dllFilePathCurrent);
        
        // Attempt to restore the original DLL name
        MoveFileEx(dllFilePathOld, dllFilePathCurrent, MOVEFILE_REPLACE_EXISTING);
        return false;
    }

    // Schedule the deletion of the old DLL
    // Since the DLL is locked by the application, we can't delete it immediately
    if (!MoveFileEx(dllFilePathOld, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        // Since its possible to execute only with admin rights, dont show error
        //snprintf(errorBuffer, errorBufferSize, "Error scheduling old DLL deletion: '%s'.", dllFilePathOld);
        //return false;
    }

    return true;
}


// Starts the update check against this fork's GitHub releases (runs on a background thread)
static void updater_sendHttpsRequest() {
    if (updater_https_checkForUpdate()) {
        Com_Printf("Auto-Updater: Checking for updates...\n");
    }
}

// This function is called when the game receives a response from the official Auto-Update server
// Original func: 0x0040ef9c CL_UpdateInfoPacket()
void updater_updatePacketResponse(struct netaddr_s addr)
{
    // 125 Hz fork: updates come only from this fork's GitHub releases, UDP update packets are ignored
    Com_DPrintf("Auto-Updater: ignored UDP update packet from %s\n", NET_AdrToString(addr));
}


// Process the update check result
static void updater_processResponse(int updateAvailable, const char* updateFile, const char* newVersionString, const char* sha256)
{
    Com_Printf("-----------------------------------\n");

    updater_waitingForResponse = false;
    updater_waitingForResponseRepeats = 0;

    // The HWID change record is left in the registry for the official CoD2x, which reports it to its server

    if (updateAvailable) {
        Com_Printf("CoD2x: Update available: %s -> %s\n", APP_VERSION, newVersionString);
        updater_expectedSha256 = sha256;

        // Server
        if (dedicated->value.boolean > 0) {
            Com_Printf("Downloading file '%s'...\n", updateFile);
            char errorBuffer[1024];
            bool ok = updater_downloadAndReplaceDllFile(updateFile, sha256, errorBuffer, sizeof(errorBuffer));
            if (ok) {
                Com_Printf("Successfully downloaded and replaced file.\n");
                Com_Printf("The update will take effect after the next server restart.\n");
            } else {
                Com_Printf("Failed to download and replace file.\n");
                Com_Printf("Error: %s\n", errorBuffer);
            }

        // Client
        } else {
            Dvar_SetBool(cl_updateAvailable, 1);
            Dvar_SetString(cl_updateFiles, updateFile);
            Dvar_SetString(cl_updateVersion, newVersionString);
            Dvar_SetString(cl_updateOldVersion, APP_VERSION);

            // Forced update
            if (updateAvailable == 2) {
                updater_forcedUpdate = true;

                if (dedicated->value.integer == 0 && clientState > CLIENT_STATE_DISCONNECTED && clientState < CLIENT_STATE_CONNECTED && demo_isPlaying == 0) {
                    updater_showForceUpdateDialog();
                }
            }
        }

    } else {
        Com_Printf("CoD2x: No updates available.\n");

        if (dedicated->value.boolean == 0) {
            Dvar_SetBool(cl_updateAvailable, 0);
        }
    }

    Com_Printf("-----------------------------------\n");
}


// Called on the main thread when the HTTPS update check has finished
static void updater_processHttpsResult(const UpdaterHttpsResult& result)
{
    if (result.ok) {
        updater_processResponse(result.available, result.url.c_str(), result.version.c_str(), result.sha256.c_str());
        return;
    }

    // 125 Hz fork: no fallback to the official update server
    Com_Printf("Auto-Updater: Request failed: %s\n", result.error.c_str());
}




// This function is called when the user confirms the update dialog
// Original func: 0x0053bc40
void updater_dialogConfirmed() {
    char errorBuffer[1024];
    bool ok = updater_downloadAndReplaceDllFile(cl_updateFiles->value.string, updater_expectedSha256.c_str(), errorBuffer, sizeof(errorBuffer));
    if (ok) {
        // Restart the application with command line arguments
        ShellExecute(NULL, "open", EXE_PATH, EXE_COMMAND_LINE, NULL, SW_SHOWNORMAL);
        ExitProcess(0);
    } else {
        Com_Error(ERR_DROP, "Failed to download and replace file.\n\n%s", errorBuffer);
    }
}



void updater_checkForUpdate() {
    // Server
    if (dedicated->value.boolean > 0) {
        // Send the request to the Auto-Update server
        if (sv_update->value.boolean && sv_running->value.boolean) {
            updater_sendHttpsRequest();
        }
    } else {
        updater_sendHttpsRequest();
    }
}


/** Called on renderer load (also after vid_restart) */
void updater_renderer() {
    updater_lastClientState = -1; // Reset last client state to ensure we check for updates on next frame
}

/** Called every frame on frame start. */
void updater_frame() {
    // If the forced update is available and player leaves main menu, show error
    if (updater_forcedUpdate && dedicated->value.integer == 0 && clientState > CLIENT_STATE_DISCONNECTED && clientState < CLIENT_STATE_CONNECTED && demo_isPlaying == 0) {
        updater_showForceUpdateDialog();
    }  
    
    // If player disconnect (so main menu is opened), check for updates again
    if (clientState != updater_lastClientState && clientState == CLIENT_STATE_DISCONNECTED) {
        updater_waitingForResponseRepeats = 0;
        updater_checkForUpdate(); // Check for updates again
    }

    // If we opened main menu while we were connected to the server, check for updates
    if (clientState == CLIENT_STATE_ACTIVE && !cg.isMainMenuClosed && cg.isMainMenuClosed != updater_lastMainMenuClosed) {
        updater_waitingForResponseRepeats = 0;
        updater_checkForUpdate(); // Check for updates again
    }

    // We did not receive a response from the Auto-Update server within 5 seconds
    if (updater_waitingForResponse && (GetTickCount() - updater_waitingForResponseTime) > 5000 && updater_waitingForResponseRepeats < 5) {
        Com_Printf("Auto-Updater: Request timed out.\n");
        // Check again for updates
        updater_waitingForResponse = false;
        updater_waitingForResponseRepeats++;
        updater_checkForUpdate();
    }

    // Result of the HTTPS update check (the request runs on a background thread)
    UpdaterHttpsResult httpsResult;
    if (updater_https_pollResult(&httpsResult)) {
        updater_processHttpsResult(httpsResult);
    }

    updater_lastClientState = clientState;
    updater_lastMainMenuClosed = cg.isMainMenuClosed; // Update last main menu closed state
}


/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void updater_init() {
    sv_update = Dvar_RegisterBool("sv_update", true, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));
}

/** Called before the entry point is called. Used to patch the memory. */
void updater_patch() {

    // Hook function that was called when update UDP packet was received from update server
    patch_call(0x0040ef9c, (unsigned int)&updater_updatePacketResponse);
    // Hook function was was called when user confirm the update dialog (it previously open url)
    patch_jump(0x0053bc40, (unsigned int)&updater_dialogConfirmed);

    // Disable original call to function that sends request to update server
    patch_nop(0x0041162f, 5);

}