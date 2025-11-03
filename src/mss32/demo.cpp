#include "demo.h"
#include "../shared/http_client.h"

#include <unordered_set>
#include <dirent.h>
#include <string>

#include "shared.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_cmd.h"
#include "../shared/cod2_shared.h"
#include "../shared/cod2_file.h"
#include "../shared/cod2_client.h"
#include "drawing.h"


#define cl_recording (*(int*)0x0064a16c)

dvar_t*        cl_demoAutoRecordName;
dvar_t*        cl_demoAutoRecordUploadUrl;
bool           demo_isUploading = false;
bool           demo_checkNextDemoForUpload = false;
HttpClient*    demo_httpClient = nullptr;
int            demo_lastClientState = -1;
size_t         demo_uploadingTotalBytes = 0;
size_t         demo_uploadingUploadedBytes = 0;
size_t         demo_uploadingBytesPerSecond = 0;
std::string    demo_uploadingErrorMessage = "";
bool           demo_uploadingDoneSuccessfully = false;
int            demo_uploadingErrorCount = 0;
uint64_t       demo_uploadingHideAtTime = 0;
bool           demo_isScheduledToCloseAfterUpload = false;
char           demo_filePath[MAX_OSPATH];
char           demo_markerFilePath[MAX_OSPATH];
char           demo_uploadUrl[1024];
std::unordered_set<std::string> demo_failedUploadMarkerPaths;



// Called if demo is being recorded and client disconnects
void cmd_StopRecord_onDisconnect() {
    ASM_CALL(RETURN_VOID, 0x0040c0c0); // Original disconnect function

    if (cl_demoAutoRecordName->value.string[0] != '\0') {
        Dvar_SetString(cl_demoAutoRecordName, ""); // Clear auto-record dvar to stop recording
    }
}


// Called from console on command "stoprecord"
// is not called when client disconnects - then original function is called directly
void cmd_StopRecord() {

    if (cl_demoAutoRecordName->value.string[0] != '\0') {
        // Prevent stopping the demo recording if auto-recording is enabled
        Com_Printf("Auto demo recording is enabled, ignoring stoprecord command.\n");
        return;
    }

    ASM_CALL(RETURN_VOID, 0x0040c0c0); // stoprecord
}

// Called from console on command "quit"
void cmd_quit() {

    // Prevent the window from closing
    if (demo_getDemoForUpload()) {
        demo_scheduleCloseAfterUpload();
        return;
    }

    ASM_CALL(RETURN_VOID, 0x004326c0);
}

void demo_scheduleCloseAfterUpload() {
    Dvar_SetString(cl_demoAutoRecordName, ""); // Clear auto-record dvar to stop recording

    demo_isScheduledToCloseAfterUpload = true;
    demo_checkNextDemoForUpload = true;
}


bool demo_getDemoForUpload(char* demoFileNameBuffer, size_t bufferSize, char* demoMarkerFileBuffer, size_t demoMarkerFileBufferSize, char* urlBuffer, size_t urlBufferSize) {
    // Check for demo upload marker files
    char demoDir[MAX_OSPATH];
    FS_BuildOSPath(demoDir, "demos", fs_gamedir, fs_homePath->value.string);

    DIR* dir;
    struct dirent* entry;

    dir = opendir(demoDir);
    if (!dir) {
        Com_Printf("Failed to open directory: %s\n", demoDir);
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat entry_stat;
        char entry_path[MAX_OSPATH];
        if (snprintf(entry_path, sizeof(entry_path), "%s/%s", demoDir, entry->d_name) >= (int)sizeof(entry_path)) {
            Com_Printf("Path too long, skipping: %s/%s\n", demoDir, entry->d_name);
            continue;
        }
        if (stat(entry_path, &entry_stat) == 0 && !S_ISDIR(entry_stat.st_mode)) {
            const char* ext = strrchr(entry->d_name, '.');
            if (ext && strcmp(ext, ".upload") == 0) {
                char filePath[MAX_OSPATH];
                if (snprintf(filePath, sizeof(filePath), "%s/%s", demoDir, entry->d_name) >= (int)sizeof(filePath)) {
                    Com_Printf("File path too long, skipping: %s/%s\n", demoDir, entry->d_name);
                    continue;
                }

                // Skip previously failed uploads
                if (demo_failedUploadMarkerPaths.find(filePath) != demo_failedUploadMarkerPaths.end()) {
                    continue;
                }

                // Process the .upload file
                Com_Printf("Found upload marker file: %s\n", filePath);

                if (demoMarkerFileBuffer && demoMarkerFileBufferSize > 0) {
                    strncpy(demoMarkerFileBuffer, filePath, demoMarkerFileBufferSize - 1);
                    demoMarkerFileBuffer[demoMarkerFileBufferSize - 1] = '\0';  // Ensure null-termination
                }

                // Copy the file path to the provided buffer
                if (demoFileNameBuffer && bufferSize > 0) {

                    // Remove .upload extension to get actual demo file name
                    size_t len = strlen(filePath);
                    if (len > 7 && strcmp(&filePath[len - 7], ".upload") == 0) {
                        filePath[len - 7] = '\0'; // Truncate the string to remove .upload
                    }

                    strncpy(demoFileNameBuffer, filePath, bufferSize - 1);
                    demoFileNameBuffer[bufferSize - 1] = '\0';  // Ensure null-termination
                }

                // Read the file contents to get the URL
                if (urlBuffer && urlBufferSize > 0) {
                    FILE* uploadFile = fopen(filePath, "r");
                    if (uploadFile) {
                        size_t bytesRead = fread(urlBuffer, 1, urlBufferSize - 1, uploadFile);
                        urlBuffer[bytesRead] = '\0'; // Null-terminate
                        fclose(uploadFile);
                    } else {
                        Com_Printf("Failed to open upload marker file for reading: %s\n", filePath);
                        urlBuffer[0] = '\0'; // Ensure empty URL on failure
                    }
                }
                
                closedir(dir);

                return true;
            }
        }
    }
    return false;
}


void demo_drawing() {

    if (demo_isScheduledToCloseAfterUpload) {
        CG_DrawRotatedPic(-128, 0, 896, 480, HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_TOP, 0, (vec4_t){0.0f, 0.0f, 0.0f, .9f}, shaderWhite);
        const char* msg = "The game will be closed after the demo upload is complete.";
        float width = UI_TextWidth(msg, INT_MAX, fontNormal, 0.3);
        float height = UI_TextHeight(fontNormal, 0.3);
        CG_DrawRotatedPic(-width / 2 - 10, -height / 2 - 5, width + 20, height + 10, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_CENTER, 0, (vec4_t){0.0f, 0.0f, 0.0f, .9f}, shaderWhite);
        UI_DrawText(msg, INT_MAX, fontNormal,
            -width / 2, +height / 2, HORIZONTAL_ALIGN_CENTER, VERTICAL_ALIGN_CENTER, 0.3f, (vec4_t){1.0f, 1.0f, 1.0f, 1.0f}, TEXT_STYLE_NORMAL);
    }

    if ((demo_isUploading || ticks_ms() < demo_uploadingHideAtTime) && (clientState == CLIENT_STATE_DISCONNECTED || clientState == CLIENT_STATE_ACTIVE)) {

        // Get file name without path and extension. Separators are both "/" and "\"
        const char* fileName = strrchr(demo_filePath, '/');
        if (!fileName) {
            fileName = strrchr(demo_filePath, '\\');
        }
        fileName = fileName ? fileName + 1 : demo_filePath;


        // Draw text in left bottom corner about uploading status
        char uploadText[256];
        double percentage = 0.0;
        vec4_t textColor;
        if (!demo_uploadingErrorMessage.empty()) {
            snprintf(uploadText, sizeof(uploadText), "Uploading demo '%s': %s", fileName, demo_uploadingErrorMessage.c_str());
            Vector4Set(textColor, 1.0f, 0.0f, 0.0f, 1.0f);
        } else if (demo_uploadingDoneSuccessfully) {
            snprintf(uploadText, sizeof(uploadText), "Uploading demo '%s': Successfully uploaded", fileName);
            Vector4Set(textColor, 0.0f, 1.0f, 0.0f, 1.0f);
        } else {
            percentage = demo_uploadingTotalBytes > 0 ? (double)demo_uploadingUploadedBytes / demo_uploadingTotalBytes * 100.0 : 0.0;
            double uploadedMB = demo_uploadingUploadedBytes / (1024.0 * 1024.0);
            double totalMB = demo_uploadingTotalBytes / (1024.0 * 1024.0);
            double speedMBps = demo_uploadingBytesPerSecond / (1024.0 * 1024.0);
            uploadText[0] = '\0';
            snprintf(uploadText, sizeof(uploadText), "Uploading demo '%s': %7.2f MB / %7.2f MB (%6.2f%%) Speed: %6.2f MB/s",
                fileName,
                uploadedMB,
                totalMB,
                percentage,
                speedMBps
            );
            Vector4Set(textColor, 1.0f, 1.0f, 1.0f, 1.0f);
        }

        float fontSize = 0.2;
        float width = UI_TextWidth(uploadText, INT_MAX, fontConsole, fontSize);
        float height = UI_TextHeight(fontConsole, fontSize);
        float padding = 4.0f;
        float offset = 0.0f;

        // Draw background bar
        vec4_t radarColorBg = { .1, .1, .1, 0.9f };
        CG_DrawRotatedPic(offset, (-offset - height - padding*2), (width + padding*2), (height + padding * 2), HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_BOTTOM, 0, radarColorBg, shaderWhite);

        // Draw progress bar
        vec4_t progressBarColor = { 0.0f, 0.0f, 1.0f, 0.2f };
        CG_DrawRotatedPic(offset, (-offset - height - padding*2), ((width + padding*2) * (percentage / 100.0f)), (height + padding * 2), HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_BOTTOM, 0, progressBarColor, shaderWhite);

        UI_DrawText(uploadText, INT_MAX, fontConsole, offset + padding, -offset - padding, HORIZONTAL_ALIGN_LEFT, VERTICAL_ALIGN_BOTTOM, fontSize, textColor, TEXT_STYLE_NORMAL);
    }
}


/** Called every frame on frame start. */
void demo_frame() {

    // Ignore for server
    if (dedicated->value.integer > 0) {
        return;
    }

    bool clientStateChanged = (demo_lastClientState != clientState);
    demo_lastClientState = clientState;

    // Mod filled name of demo to auto record
    if (cl_demoAutoRecordName->modified) {
        cl_demoAutoRecordName->modified = false;

        // Reset error count on new recording
        demo_uploadingErrorCount = 0;

        const char* demoName = cl_demoAutoRecordName->value.string;
        if (cl_recording) {
            ASM_CALL(RETURN_VOID, 0x0040c0c0); // stoprecord
        }
        if (demoName && demoName[0] != '\0') {
            
            char newDemoName[MAX_OSPATH];
            char qpath[MAX_OSPATH];
            int suffix = 0;

            // Replace invalid characters in the demo name with underscores
            const char* invalidChars = "\\/:*?\"<>|";
            strncpy(newDemoName, demoName, sizeof(newDemoName) - 1);
            newDemoName[sizeof(newDemoName) - 1] = '\0';

            for (char* p = newDemoName; *p; ++p) {
                if (strchr(invalidChars, *p)) {
                    *p = '_';
                }
            }

            // Check for existing file and add suffix if necessary
            do {
                if (suffix > 0) {
                    snprintf(newDemoName, sizeof(newDemoName), "%s_%d", demoName, suffix);
                }
                snprintf(qpath, sizeof(qpath), "demos/%s.dm_1", newDemoName);
                suffix++;
            } while (FS_FileExists(qpath));

            // Set the dvar to the new unique name
            Dvar_SetString(cl_demoAutoRecordName, newDemoName);
            cl_demoAutoRecordName->modified = false;

            Cmd_ExecuteString(va("record %s\n", newDemoName)); // execute immediately

            if (cl_recording && cl_demoAutoRecordUploadUrl && cl_demoAutoRecordUploadUrl->value.string[0] != '\0') {
                // Create helper file that indicated the demo needs to be uploaded later
                char url[1024];
                snprintf(url, sizeof(url), "%s%s", cl_demoAutoRecordUploadUrl->value.string, newDemoName);
                FS_WriteFile(va("demos/%s.dm_1.upload", newDemoName), url, strlen(url));
            }
        }

        demo_checkNextDemoForUpload = true;
    }


    // Check for demos to upload
    bool checkDemosForUpload = !cl_recording && !demo_isUploading && ticks_ms() > demo_uploadingHideAtTime;
    if (checkDemosForUpload && (clientState == CLIENT_STATE_DISCONNECTED || clientState == CLIENT_STATE_ACTIVE) && (clientStateChanged || demo_checkNextDemoForUpload)) {

        demo_checkNextDemoForUpload = false;

        do {
            bool demo_found = demo_getDemoForUpload(demo_filePath, sizeof(demo_filePath), demo_markerFilePath, sizeof(demo_markerFilePath), demo_uploadUrl, sizeof(demo_uploadUrl));
            if (!demo_found) break;

            // Check if the demo file exists and is readable before opening
            if (access(demo_filePath, R_OK) != 0) {
                Com_Printf("Demo file not found or not readable, removing marker file: '%s'\n", demo_markerFilePath);
                if (remove(demo_markerFilePath) != 0) {
                    Com_Error(ERR_FATAL, "Failed to delete marker file: %s\n", demo_markerFilePath);
                }
                continue;
            }

            FILE* demoFile = fopen(demo_filePath, "rb");
            if (!demoFile) {
                Com_Error(ERR_FATAL, "Failed to open demo file: %s\n", demo_filePath);
                continue;
            }

            // Get the file size
            fseek(demoFile, 0, SEEK_END);
            long fileSize = ftell(demoFile);
            rewind(demoFile);

            if (fileSize <= 0) {
                // Remove the marker file since the demo file is invalid
                Com_Printf("Demo file is empty or invalid, deleting marker file: %s\n", demo_markerFilePath);
                fclose(demoFile);
                if (remove(demo_markerFilePath) != 0) {
                    Com_Error(ERR_FATAL, "Failed to delete marker file: %s\n", demo_markerFilePath);
                }
                continue;
            }

            // Read marker file to get upload URL
            char url[1024] = {0};
            FILE* markerFile = fopen(demo_markerFilePath, "r");
            if (!markerFile) {
                Com_Error(ERR_FATAL, "Failed to open marker file for reading: %s\n", demo_markerFilePath);
                continue;
            }
            if (!fgets(url, sizeof(url), markerFile)) {
                fclose(markerFile);
                Com_Error(ERR_FATAL, "Failed to read URL from marker file: %s\n", demo_markerFilePath);
                continue;
            }
            // Remove newline characters
            url[strcspn(url, "\r\n")] = 0;
            fclose(markerFile);

            // If URL is not valid, remove marker file
            if (!HttpClient::is_valid_url(url)) {
                Com_Printf("Invalid upload URL in marker file, removing marker file: '%s'\n", demo_markerFilePath);
                if (remove(demo_markerFilePath) != 0) {
                    Com_Error(ERR_FATAL, "Failed to delete marker file: %s\n", demo_markerFilePath);
                }
                fclose(demoFile);
                continue;
            }

            demo_isUploading = true;
            if (demo_httpClient == nullptr)
                demo_httpClient = new HttpClient();

            demo_uploadingUploadedBytes = 0;
            demo_uploadingTotalBytes = fileSize;
            demo_uploadingBytesPerSecond = 0;
            demo_uploadingErrorMessage = "";
            demo_uploadingDoneSuccessfully = false;

            // Get demo file name. Separators are both "/" and "\"
            const char* fileName = strrchr(demo_filePath, '/');
            if (!fileName) {
                fileName = strrchr(demo_filePath, '\\');
            }
            fileName++;

            demo_httpClient->upload_chunks(
                url,
                fileSize,
                [demoFile](char* buf, size_t maxLen, size_t offset) -> size_t {
                    if (fseek(demoFile, (long)offset, SEEK_SET) != 0) return 0;
                    return fread(buf, 1, maxLen, demoFile);
                },
                [](size_t uploaded, size_t total, size_t bytes_per_second) {
                    //double percentage = total > 0 ? (double)uploaded / total * 100.0 : 0.0;
                    //Com_Printf("Demo upload progress: %zu/%zu bytes (%.1f%%) Speed: %.2f MB/s\n", uploaded, total, percentage, bytes_per_second / (1024.0 * 1024.0));
                    demo_uploadingUploadedBytes = uploaded;
                    demo_uploadingTotalBytes = total;
                    demo_uploadingBytesPerSecond = bytes_per_second;
                },
                [demoFile](const HttpClient::Response& response) {
                    Com_Printf("Demo upload completed! Status: %d\n", response.status);
                    if (response.status == 200 || response.status == 201 || response.status == 409) { // 409 = Conflict (demo already exists)
                        Com_Printf("Demo upload successful with status: %d!\n", response.status);
                        // Delete the .upload marker file
                        if (DEBUG) {
                            if (rename(demo_markerFilePath, va("%s.old", demo_markerFilePath)) != 0) {
                                Com_Error(ERR_FATAL, "Failed to rename marker file: %s\n", demo_markerFilePath);
                            }
                        } else {
                            if (remove(demo_markerFilePath) != 0) {
                                Com_Error(ERR_FATAL, "Failed to delete marker file: %s\n", demo_markerFilePath);
                            }
                        }
                        demo_uploadingHideAtTime = ticks_ms() + 2000; // Hide after 2 seconds
                        demo_uploadingDoneSuccessfully = true;

                    } else {
                        Com_Printf("Demo upload failed with status: %d\n", response.status);
                        demo_uploadingErrorMessage = "Error: " + std::to_string(response.status);
                        demo_uploadingHideAtTime = ticks_ms() + 3000; // Hide after 3 seconds
                        demo_uploadingErrorCount++;
                        if (demo_uploadingErrorCount >= 3) {
                            demo_uploadingErrorCount = 0;
                            demo_failedUploadMarkerPaths.insert(demo_markerFilePath);
                        }
                    }
                    fclose(demoFile);
                    demo_isUploading = false;
                    demo_checkNextDemoForUpload = true;
                },
                [demoFile](const std::string& error) {
                    Com_Printf("Demo upload error: %s\n", error.c_str());
                    demo_uploadingErrorMessage = "Error: " + error;
                    demo_uploadingHideAtTime = ticks_ms() + 3000; // Hide after 3 seconds
                    demo_uploadingErrorCount++;
                    if (demo_uploadingErrorCount >= 3) {
                        demo_uploadingErrorCount = 0;
                        demo_failedUploadMarkerPaths.insert(demo_markerFilePath);
                    }
                    fclose(demoFile);
                    demo_isUploading = false;
                    demo_checkNextDemoForUpload = true;
                },
                60000,
                2000,
                5 * 1024 * 1024 // 5MB/s bandwidth limit
            );

            break; // Process one file at a time
        } while(0);
    }

    // If we disconnected, clear cvars
    if (clientStateChanged && clientState == CLIENT_STATE_DISCONNECTED) {
        Dvar_SetString(cl_demoAutoRecordName, ""); // Clear auto-record dvar to stop recording
        Dvar_SetString(cl_demoAutoRecordUploadUrl, ""); // Clear upload URL dvar
    }

    if (demo_httpClient != nullptr)
        demo_httpClient->poll();

    // Close the game if scheduled after upload is complete
    if (demo_isScheduledToCloseAfterUpload && !demo_isUploading && ticks_ms() > demo_uploadingHideAtTime) {
        Com_Printf("Demo upload complete, closing the game now.\n");
        Cmd_ExecuteString("quit\n");
    }
}

void demo_unload() {
    if (demo_httpClient) {
        delete demo_httpClient;
        demo_httpClient = nullptr;
    }
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void demo_init() {
    cl_demoAutoRecordName = Dvar_RegisterString("cl_demoAutoRecordName", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET | DEBUG_RELEASE(DVAR_NOFLAG, DVAR_NOWRITE)));
    cl_demoAutoRecordUploadUrl = Dvar_RegisterString("cl_demoAutoRecordUploadUrl", "", (dvarFlags_e)(DVAR_CHANGEABLE_RESET | DEBUG_RELEASE(DVAR_NOFLAG, DVAR_NOWRITE)));

    #if DEBUG && 0
    //Dvar_SetString(cl_demoAutoRecordUploadUrl, "http://localhost:8080/api/match/12345/demo-upload/");
    //Dvar_SetString(cl_demoAutoRecordUploadUrl, "http://master.cod2x.me/api/match/12345/demo-upload/");
    #endif
}

/** Called before the entry point is called. Used to patch the memory. */
void demo_patch() {
    patch_int32(0x004346b1 + 1, (int32_t)&cmd_quit);
    patch_int32(0x00411430 + 1, (int32_t)&cmd_StopRecord);
    patch_call(0x0040cd2e, (unsigned int)&cmd_StopRecord_onDisconnect);
}
