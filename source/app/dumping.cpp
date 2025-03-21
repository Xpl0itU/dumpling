#include "dumping.h"
#include "progress.h"
#include "menu.h"
#include "filesystem.h"
#include "navigation.h"
#include "titles.h"
#include "users.h"
#include "cfw.h"
#include "gui.h"
#include "LockingQueue.h"
#include <future>

#include <mocha/mocha.h>
#include <mocha/otp.h>

// Dumping Functions

#define BUFFER_SIZE_ALIGNMENT 64
#define BUFFER_SIZE (1024 * BUFFER_SIZE_ALIGNMENT)
static_assert(BUFFER_SIZE % 0x8000 == 0, "Buffer size needs to be multiple of 0x8000 to properly read disc sections");

static uint8_t* copyBuffer = nullptr;
static bool cancelledScanning = false;
static bool ignoreFileErrors = false;

typedef struct {
    void *buf;
    size_t len;
    size_t buf_size;
} file_buffer;

static file_buffer buffers[16];
static bool buffersInitialized = false;

bool copyMemory(uint8_t* srcBuffer, uint64_t bufferSize, std::string destPath, uint64_t* totalBytes) {
    if (copyBuffer == nullptr) {
        // Allocate buffer to copy bytes between
        copyBuffer = (uint8_t*)aligned_alloc(BUFFER_SIZE_ALIGNMENT, BUFFER_SIZE);
        if (copyBuffer == nullptr) {
            setErrorPrompt("Couldn't allocate the memory to copy files!");
            return false;
        }
    }

    // Create parent folder if doesn't exist yet
    std::filesystem::path filePath(destPath);
    createPath(filePath.parent_path().c_str());

    // If totalBytes is set it means that it's scanning for file sizes of openable files
    if (totalBytes != nullptr) {
        *totalBytes += bufferSize;

        // Check whether user has cancelled scanning
        updateInputs();
        if (pressedBack()) {
            *totalBytes = 0;
            cancelledScanning = true;
            return false;
        }
        return true;
    }

    // Open the destination file
    FILE* writeHandle = fopen(destPath.c_str(), "wb");
    if (writeHandle == nullptr) {
        reportFileError();
        if (ignoreFileErrors) return true;
        std::string errorMessage = "Couldn't open the file to copy to!\n";
        errorMessage += "For SD cards: Make sure it isn't locked\nby the write-switch on the side!\n\nDetails:\n";
        errorMessage += "Error "+std::to_string(errno)+" after creating SD card file:\n";
        errorMessage += destPath + "\n";
        errorMessage += "to:\n";
        errorMessage += destPath;
        setErrorPrompt(errorMessage);
        return false;
    }

    setFile(filePath.filename().c_str(), bufferSize);

    // Copy source buffer into aligned copy buffer, then write it in parts
    // todo: Check if this is done properly regarding edges of source and copy buffer sizes
    for (uint64_t i=0; i<bufferSize; i+=BUFFER_SIZE) {
        size_t bytesToWrite = std::min(bufferSize-i, (uint64_t)BUFFER_SIZE);
        memcpy(copyBuffer, srcBuffer, bytesToWrite);
        
        size_t bytesWritten = fwrite(copyBuffer, sizeof(uint8_t), bytesToWrite, writeHandle);

        // Check if the same amounts of bytes are written
        if (bytesWritten != bytesToWrite) {
            setErrorPrompt("Something went wrong during the fily copy where not all bytes were written!");
            fclose(writeHandle);
            return false;
        }
        setFileProgress(bytesWritten);
        showCurrentProgress();
        // Check whether the inputs break
        updateInputs();
        if (pressedBack()) {
            uint8_t selectedChoice = showDialogPrompt("Are you sure that you want to cancel the dumping process?", "Yes", "No");
            if (selectedChoice == 0) {
                setErrorPrompt("Couldn't delete files from SD card, please delete them manually.");
                fclose(writeHandle);
                return false;
            }
        }
    }
    fclose(writeHandle);
    return true;
}

static bool readThread(FILE *srcFile, LockingQueue<file_buffer> *ready, LockingQueue<file_buffer> *done) {
    file_buffer currentBuffer;
    ready->waitAndPop(currentBuffer);
    while ((currentBuffer.len = fread(currentBuffer.buf, 1, currentBuffer.buf_size, srcFile)) > 0) {
        done->push(currentBuffer);
        ready->waitAndPop(currentBuffer);
    }
    done->push(currentBuffer);
    return ferror(srcFile) == 0;
}

static bool writeThread(FILE *dstFile, LockingQueue<file_buffer> *ready, LockingQueue<file_buffer> *done, size_t totalSize) {
    uint bytes_written;
    size_t total_bytes_written = 0;
    file_buffer currentBuffer;
    ready->waitAndPop(currentBuffer);
    while (currentBuffer.len > 0 && (bytes_written = fwrite(currentBuffer.buf, 1, currentBuffer.len, dstFile)) == currentBuffer.len) {
        done->push(currentBuffer);
        ready->waitAndPop(currentBuffer);
        total_bytes_written += bytes_written;
        setFileProgress(total_bytes_written);
        showCurrentProgress();
        // Check whether the inputs break
        updateInputs();
        if (pressedBack()) {
            uint8_t selectedChoice = showDialogPrompt("Are you sure that you want to cancel the dumping process?", "Yes", "No");
            WHBLogFreetypeClear();
            WHBLogPrint("Quitting dumping process...");
            WHBLogPrint("The app (likely) isn't frozen!");
            WHBLogPrint("This should take a minute at most!");
            WHBLogFreetypeDraw();
            if (selectedChoice == 0) {
                setErrorPrompt("Couldn't delete files from SD card, please delete them manually.");
                return false;
            }
        }
    }
    done->push(currentBuffer);
    return ferror(dstFile) == 0;
}

static bool copyFileThreaded(FILE *srcFile, FILE *dstFile, size_t totalSize) {
    LockingQueue<file_buffer> read;
    LockingQueue<file_buffer> write;
    for (auto &buffer : buffers) {
        if (!buffersInitialized) {
            buffer.buf = aligned_alloc(0x40, BUFFER_SIZE);
            buffer.len = 0;
            buffer.buf_size = BUFFER_SIZE;
        }
        read.push(buffer);
    }

    std::future<bool> readFut = std::async(std::launch::async, readThread, srcFile, &read, &write);
    std::future<bool> writeFut = std::async(std::launch::async, writeThread, dstFile, &write, &read, totalSize);
    bool success = readFut.get() && writeFut.get();
    return success;
}

bool copyFile(const char* filename, std::string srcPath, std::string destPath, uint64_t* totalBytes) {
    // Check if file is an actual file first
    struct stat fileStat;
    if (lstat(srcPath.c_str(), &fileStat) == -1) {
        reportFileError();
        if (ignoreFileErrors) return true;
        std::string errorMessage = "Failed to retrieve info from source file!\n";
        if (errno == EIO) errorMessage += "For discs: Make sure that it's clean!\nDumping is very sensitive to tiny issues!\n";
        errorMessage += "\nDetails:\n";
        errorMessage += "Error "+std::to_string(errno)+" when getting stats for:\n";
        errorMessage += srcPath;
        setErrorPrompt(errorMessage);
        return false;
    }

    // If totalBytes is set it means that it's scanning for file sizes of openable files
    if (totalBytes != nullptr) {
        *totalBytes += fileStat.st_size;

        // Check whether user has cancelled scanning
        updateInputs();
        if (pressedBack()) {
            *totalBytes = 0;
            cancelledScanning = true;
            return false;
        }
        return true;
    }

    if (copyBuffer == nullptr) {
        // Allocate buffer to copy bytes between
        copyBuffer = (uint8_t*)aligned_alloc(BUFFER_SIZE_ALIGNMENT, BUFFER_SIZE);
        if (copyBuffer == nullptr) {
            setErrorPrompt("Couldn't allocate the memory to copy files!");
            return false;
        }
    }

    FILE* readHandle = fopen(srcPath.c_str(), "rb");
    if (readHandle == nullptr) {
        reportFileError();
        if (ignoreFileErrors) return true;
        std::string errorMessage = "Couldn't open the file to copy from!\n";
        if (errno == EIO) errorMessage += "For discs: Make sure that it's clean!\nDumping is very sensitive to tiny errors!\n";
        errorMessage += "\nDetails:\n";
        errorMessage += "Error "+std::to_string(errno)+" after opening file!\n";
        errorMessage += srcPath;
        setErrorPrompt(errorMessage);
        return false;
    }

    // Open the destination file
    FILE* writeHandle = fopen(destPath.c_str(), "wb");
    if (writeHandle == nullptr) {
        fclose(readHandle);
        reportFileError();
        if (ignoreFileErrors) return true;
        std::string errorMessage = "Couldn't open the file to copy to!\n";
        errorMessage += "For SD cards: Make sure it isn't locked\nby the write-switch on the side!\n\nDetails:\n";
        errorMessage += "Error "+std::to_string(errno)+" after creating SD card file:\n";
        errorMessage += srcPath + "\n";
        errorMessage += "to:\n";
        errorMessage += destPath;
        setErrorPrompt(errorMessage);
        return false;
    }

    setFile(filename, fileStat.st_size);
    
    if(!copyFileThreaded(readHandle, writeHandle, fileStat.st_size))
        return false;

    fclose(readHandle);
    fclose(writeHandle);
    return true;
}

bool copyFolder(std::string srcPath, std::string destPath, uint64_t* totalBytes) {
    // Open folder
    DIR* dirHandle;
    if ((dirHandle = opendir((srcPath+"/").c_str())) == nullptr) {
        reportFileError();
        if (ignoreFileErrors) return true;
        std::string errorMessage = "Failed to open folder to read content from!\n";
        if (errno == EIO) errorMessage += "For discs: Make sure that it's clean!\nDumping is very sensitive to tiny issues!\n";
        errorMessage += "\nDetails:\n";
        errorMessage += "Error "+std::to_string(errno)+" while opening folder:\n";
        errorMessage += srcPath;
        setErrorPrompt(errorMessage);
        setErrorPrompt("Failed to open the directory to read files from");
        return false;
    }

    // Append slash when last character isn't a slash
    if (totalBytes == nullptr) createPath(destPath.c_str());

    // Loop over directory contents
    while (true) {
        errno = 0;
        struct dirent* dirEntry = readdir(dirHandle);
        if (dirEntry != NULL) {
            std::string entrySrcPath = srcPath + "/" + dirEntry->d_name;
            std::string entryDstPath = destPath + "/" + dirEntry->d_name;
            // Use lstat since readdir returns DT_REG for symlinks
            struct stat fileStat;
            if (lstat(entrySrcPath.c_str(), &fileStat) != 0) {
                reportFileError();
                if (ignoreFileErrors) continue;
                setErrorPrompt("Unknown Error "+std::to_string(errno)+":\nCouldn't check type of file/folder!\n"+entrySrcPath);
                return false;
            }
            
            if (S_ISLNK(fileStat.st_mode)) {
                continue;
            }
            else if (S_ISREG(fileStat.st_mode)) {
                // Copy file
                if (!copyFile(dirEntry->d_name, entrySrcPath, entryDstPath, totalBytes)) {
                    closedir(dirHandle);
                    return false;
                }
            }
            else if (S_ISDIR(fileStat.st_mode)) {
                // Ignore root and parent folder entries
                if (strcmp(dirEntry->d_name, ".") == 0 || strcmp(dirEntry->d_name, "..") == 0) continue;

                // Copy all the files in this subdirectory
                if (!copyFolder(entrySrcPath, entryDstPath, totalBytes)) {
                    closedir(dirHandle);
                    return false;
                }
            }
        }
        else if (dirEntry == NULL && errno == EIO) {
            reportFileError();
            std::string errorMessage = "The Wii U had an issue while reading the disc(?)!\n";
            errorMessage += "Make sure that it's clean!\nDumping is very sensitive to tiny issues!\n";
            errorMessage += "Unfortunately, disc requires reinsertion to retry!\nTry restarting Dumpling and trying again.\n\nDetails:\n";
            errorMessage += "Error "+std::to_string(errno)+" while iterating folder contents:\n";
            errorMessage += srcPath;
            setErrorPrompt(errorMessage);
            return false;
        }
        else if (dirEntry == NULL && errno != 0) {
            reportFileError();
            if (ignoreFileErrors) continue;
            std::string errorMessage = "Unknown error while reading disc contents!\n";
            errorMessage += "You might want to restart your console and try again.\n\nDetails:\n";
            errorMessage += "Error "+std::to_string(errno)+" while iterating folder contents:\n";
            errorMessage += srcPath;
            setErrorPrompt(errorMessage);
            return false;
        }
        else {
            break;
        }
    }

    closedir(dirHandle);
    return true;
}

bool dumpTitle(titleEntry& entry, dumpingConfig& config, uint64_t* totalBytes) {
    if (HAS_FLAG(config.dumpTypes, dumpTypeFlags::Game) && entry.base) {
        if (!copyFolder(entry.base->posixPath, getRootFromLocation(config.location)+"/dumpling"+entry.base->outputPath, totalBytes)) return false;
    }
    if (HAS_FLAG(config.dumpTypes, dumpTypeFlags::Update) && entry.update) {
        if (!copyFolder(entry.update->posixPath, getRootFromLocation(config.location)+"/dumpling"+entry.update->outputPath, totalBytes)) return false;
    }
    if (HAS_FLAG(config.dumpTypes, dumpTypeFlags::DLC) && entry.dlc) {
        if (!copyFolder(entry.dlc->posixPath, getRootFromLocation(config.location)+"/dumpling"+entry.dlc->outputPath, totalBytes)) return false;
    }
    if (HAS_FLAG(config.dumpTypes, dumpTypeFlags::Saves) && entry.saves && (entry.saves->commonSave || !entry.saves->userSaves.empty())) {
        for (auto& save : entry.saves->userSaves) {
            if (save.userId == config.accountId) {
                if (!copyFolder(save.path, getRootFromLocation(config.location)+"/dumpling/Saves/"+entry.folderName+"/"+(config.dumpAsDefaultUser?"80000001":getUserByPersistentId(save.userId)->persistentIdString), totalBytes)) return false;
            }
        }
        if (entry.saves->commonSave) {
            if (!copyFolder(entry.saves->commonSave->path, getRootFromLocation(config.location)+"/dumpling/Saves/"+entry.folderName+"/common", totalBytes)) return false;
        }
    }
    if (HAS_FLAG(config.dumpTypes, dumpTypeFlags::Custom) && entry.custom) {
        if (entry.custom->inputPath) {
            if (!copyFolder(*entry.custom->inputPath, getRootFromLocation(config.location)+"/dumpling"+entry.custom->outputPath, totalBytes)) return false;
        }
        if (entry.custom->inputBuffer) {
            if (!copyMemory(entry.custom->inputBuffer->first, entry.custom->inputBuffer->second, getRootFromLocation(config.location)+"/dumpling"+entry.custom->outputPath, totalBytes)) return false;
        }
    }
    return true;
}

bool dumpQueue(std::vector<std::reference_wrapper<titleEntry>>& queue, dumpingConfig& config) {
    // Ask user whether it wants to do an initial scan
    uint8_t scanChoice = showDialogPrompt("Run an initial scan to determine the dump size?\nThis might take some minutes but adds:\n - Progress while dumping\n - Check if enough storage is available\n - Give a rough time estimate", "Yes, check size", "No, just dump");
    
    uint64_t totalDumpSize = 0;
    cancelledScanning = false;
    if (scanChoice == 0) {
        // Scan folder to get the full queue size
        for (uint64_t i=0; i<queue.size(); i++) {
            // Show message about the scanning process which freezes the game
            WHBLogFreetypeStartScreen();
            WHBLogPrint("Scanning the dump size!");
            WHBLogPrint("This might take a few minutes if you selected a lot of titles...");
            WHBLogPrint("Your Wii U isn't frozen in that case!");
            WHBLogPrint("");
            WHBLogPrintf("Scanning %s... (title %lu / %lu)", queue[i].get().shortTitle.c_str(), i+1, queue.size());
            WHBLogPrint("");
            WHBLogFreetypeScreenPrintBottom("===============================");
            WHBLogFreetypeScreenPrintBottom("\uE001 Button = Cancel scanning and just do dumping");
            WHBLogFreetypeDraw();
            if (!dumpTitle(queue[i], config, &totalDumpSize) && !cancelledScanning) {
                showErrorPrompt("Exit to Main Menu");
                showDialogPrompt("Failed while trying to scan the dump for its size!", "Exit to Main Menu");
                return false;
            }
        }
    }

    if (totalDumpSize != 0) {
        // Check if there's enough space on the storage location and otherwise give a warning
        uint64_t sizeAvailable = getFreeSpace(getRootFromLocation(config.location).c_str());
        if (sizeAvailable < totalDumpSize) {
            std::string freeSpaceWarning;
            freeSpaceWarning += "You only have ";
            freeSpaceWarning += formatByteSize(sizeAvailable);
            freeSpaceWarning += ", while you require ";
            freeSpaceWarning += formatByteSize(totalDumpSize);
            freeSpaceWarning += " of free space!\n";
            if (sizeAvailable == 0) freeSpaceWarning += "Make sure that your storage device is still plugged in!";
            else freeSpaceWarning += "Make enough space, or dump one game at a time.";
            showDialogPrompt(freeSpaceWarning.c_str(), "Exit to Main Menu");
            return false;
        }
        else {
            WHBLogFreetypeClear();
            WHBLogPrintf("Dump is %s while selected location has %s available!", formatByteSize(totalDumpSize).c_str(), formatByteSize(sizeAvailable).c_str());
            WHBLogPrint("Dumping will start in 10 seconds...");
            WHBLogFreetypeDraw();
            sleep_for(10s);
        }
    }

    // Dump title
    startQueue(totalDumpSize);
    ignoreFileErrors = config.ignoreCopyErrors;
    for (size_t i=0; i<queue.size(); i++) {
        std::string status("Currently dumping ");
        status += queue[i].get().shortTitle;
        if (queue.size() > 1) {
            if (HAS_FLAG(config.filterTypes, dumpTypeFlags::Game)) status += " (game ";
            else if (HAS_FLAG(config.filterTypes, dumpTypeFlags::Update)) status += " (update ";
            else if (HAS_FLAG(config.filterTypes, dumpTypeFlags::DLC)) status += " (DLC ";
            else if (HAS_FLAG(config.filterTypes, dumpTypeFlags::Saves)) status += " (save ";
            else if (HAS_FLAG(config.filterTypes, dumpTypeFlags::SystemApp)) status += " (app ";
            else if (HAS_FLAG(config.filterTypes, dumpTypeFlags::Custom)) status += " (custom ";
            else status += " (title ";
            status += std::to_string(i+1);
            status += "/";
            status += std::to_string(queue.size());
            status += ")";
        }
        status += "...";

        setDumpingStatus(status);
        if (!dumpTitle(queue[i], config, nullptr)) {
            ignoreFileErrors = false;
            showErrorPrompt("Back to Main Menu");
            return false;
        }
    }
    ignoreFileErrors = false;
    return true;
}

void dumpMLC() {
    dumpingConfig mlcConfig = {.dumpTypes = dumpTypeFlags::Custom};
    titleEntry mlcEntry{.shortTitle = "MLC Dump", .custom = customPart{.inputPath = convertToPosixPath("/vol/storage_mlc01/"), .outputPath = "/MLC Dump"}};
    
    uint8_t selectedChoice = showDialogPrompt("Dumping the MLC can take 6-20 hours depending on it's contents!\nMake sure that you have enough space available.", "Proceed", "Cancel");
    if (selectedChoice == 1) return;

    // Show the option screen
    if (!showOptionMenu(mlcConfig, false)) {
        return;
    }

    setDumpingStatus("Currently dumping the whole MLC...");
    if (!dumpTitle(mlcEntry, mlcConfig, nullptr)) {
        showErrorPrompt("Back to Main Menu");
        return;
    }
}

bool dumpDisc() {
    int32_t mcpHandle = MCP_Open();
    if (mcpHandle < 0) {
        WHBLogPrint("Failed to open MPC to check for inserted disc titles");
        return false;
    }

    if (!checkForDiscTitles(mcpHandle)) {
        // Loop until a disk is found
        while(true) {
            WHBLogFreetypeStartScreen();
            WHBLogPrint("Looking for a game disc...");
            WHBLogPrint("Please insert one if you haven't already!");
            WHBLogPrint("");
            WHBLogPrint("Reinsert the disc if you started Dumpling");
            WHBLogPrint("with a disc already inserted!");
            WHBLogPrint("");
            WHBLogFreetypeScreenPrintBottom("===============================");
            WHBLogFreetypeScreenPrintBottom("\uE001 Button = Back to Main Menu");
            WHBLogFreetypeDrawScreen();
            sleep_for(100ms);

            updateInputs();
            if (pressedBack()) {
                MCP_Close(mcpHandle);
                return true;
            }
            if (checkForDiscTitles(mcpHandle)) {
                break;
            }
        }
    }
    MCP_Close(mcpHandle);

    // Scan disc titles this time
    WHBLogFreetypeClear();
    WHBLogPrint("Refreshing games list:");
    WHBLogPrint("");
    WHBLogFreetypeDraw();

    if (!mountDisc()) {
        showDialogPrompt("Error while mounting disc!", "OK");
        return false;
    }

    if (!loadTitles(false)) {
        showDialogPrompt("Error while scanning disc titles!", "OK");
        return false;
    }

    // Make a queue from game disc
    std::vector<std::reference_wrapper<titleEntry>> queue;
    for (auto& title : installedTitles) {
        if (title.base && title.base->location == titleLocation::Disc) {
            queue.emplace_back(std::ref(title));
            break;
        }
    }

    WHBLogFreetypeClear();
    WHBLogPrint("Currently inserted disc is:");
    WHBLogPrint(queue.begin()->get().shortTitle.c_str());
    WHBLogPrint("");
    WHBLogPrint("Continuing to next step in 5 seconds...");
    WHBLogFreetypeDraw();
    sleep_for(5s);

    // Dump queue
    uint8_t dumpOnlyBase = showDialogPrompt("Do you want to dump the update and DLC files?\nIt might be faster to dump these:\n - All updates/DLC at once, without disc swapping!\n - Through methods like Cemu's Online Features, DumpsterU etc.", "Yes", "No");
    dumpingConfig config = {.dumpTypes = (dumpOnlyBase == 0) ? (dumpTypeFlags::Game | dumpTypeFlags::Saves) : (dumpTypeFlags::Game | dumpTypeFlags::Update | dumpTypeFlags::DLC | dumpTypeFlags::Saves)};
    if (!showOptionMenu(config, true)) return true;
    if (dumpQueue(queue, config)) {
        showDialogPrompt("Successfully dumped this disc!", "OK");
        return true;
    }
    else {
        showDialogPrompt("Failed to dump disc files...", "OK");
        return false;
    }
}

void dumpOnlineFiles() {
    std::vector<std::reference_wrapper<titleEntry>> queue;
    dumpingConfig onlineConfig = {.dumpTypes = (dumpTypeFlags::Custom)};

    // Loop until a valid account has been chosen
    std::string accountIdStr = "";
    std::string accountNameStr = "";
    while(accountIdStr.empty()) {
        if (!showOptionMenu(onlineConfig, true)) return;
        
        // Check if the selected user has 
        for (auto& user : allUsers) {
            if (user.persistentId == onlineConfig.accountId) {
                if (!user.networkAccount) showDialogPrompt("This account doesn't have a NNID connected to it!\n\nSteps on how to connect/create a NNID to your Account:\n - Click the Mii icon on the Wii U's homescreen.\n - Click on the Link a Nintendo Network ID option.\n - Return to Dumpling.", "OK");
                else if (!user.passwordCached) showDialogPrompt("Your password isn't saved!\n\nSteps on how to save your password in your account:\n - Click your Mii icon on the Wii U's homescreen.\n - Enable the Save Password option.\n - Return to Dumpling.", "OK");
                else {
                    accountIdStr = user.persistentIdString;
                    accountNameStr = normalizeFolderName(user.miiName);
                    trim(accountNameStr);
                    if (accountNameStr.empty()) accountNameStr = "Non-Ascii User Name ["+accountIdStr+"]";
                }
            }
        }
    }

    titleEntry ecAccountInfoEntry{.shortTitle = "eShop Key File", .custom = customPart{.inputPath = convertToPosixPath("/vol/storage_mlc01/usr/save/system/nim/ec/"), .outputPath = "/Online Files/"+accountNameStr+"/mlc01/usr/save/system/nim/ec"}};
    titleEntry miiEntry{.shortTitle = "Mii Files", .custom = customPart{.inputPath = convertToPosixPath("/vol/storage_mlc01/sys/title/0005001b/10056000/content"), .outputPath = "/Online Files/"+accountNameStr+"/mlc01/sys/title/0005001b/10056000/content"}};
    titleEntry ccertsEntry{.shortTitle = "ccerts Files", .custom = customPart{.inputPath = convertToPosixPath("/vol/storage_mlc01/sys/title/0005001b/10054000/content/ccerts"), .outputPath = "/Online Files/"+accountNameStr+"/mlc01/sys/title/0005001b/10054000/content/ccerts"}};
    titleEntry scertsEntry{.shortTitle = "scerts Files", .custom = customPart{.inputPath = convertToPosixPath("/vol/storage_mlc01/sys/title/0005001b/10054000/content/scerts"), .outputPath = "/Online Files/"+accountNameStr+"/mlc01/sys/title/0005001b/10054000/content/scerts"}};
    titleEntry accountsEntry{.shortTitle = "Selected Account", .custom = customPart{.inputPath = convertToPosixPath((std::string("/vol/storage_mlc01/usr/save/system/act/")+accountIdStr).c_str()), .outputPath = "/Online Files/"+accountNameStr+"/mlc01/usr/save/system/act/"+(onlineConfig.dumpAsDefaultUser ? "80000001" : accountIdStr)}};

    // Dump otp.bin and seeprom.bin
    WiiUConsoleOTP otpReturn;
    std::vector<uint8_t> seepromBuffer(512);

    Mocha_ReadOTP(&otpReturn);
    Mocha_SEEPROMRead(seepromBuffer.data(), 0, seepromBuffer.size());

    titleEntry seepromFile{.shortTitle = "seeprom.bin File", .custom = customPart{.inputBuffer = std::pair((uint8_t*)seepromBuffer.data(), seepromBuffer.size()), .outputPath = "/Online Files/seeprom.bin"}};
    titleEntry otpFile{.shortTitle = "otp.bin File", .custom = customPart{.inputBuffer = std::pair((uint8_t*)&otpReturn, sizeof(otpReturn)), .outputPath = "/Online Files/otp.bin"}};

    // Add custom title entries to queue
    queue.emplace_back(std::ref(ecAccountInfoEntry));
    queue.emplace_back(std::ref(miiEntry));
    queue.emplace_back(std::ref(ccertsEntry));
    queue.emplace_back(std::ref(scertsEntry));
    queue.emplace_back(std::ref(accountsEntry));
    queue.emplace_back(std::ref(seepromFile));
    queue.emplace_back(std::ref(otpFile));

    if (dumpQueue(queue, onlineConfig)) showDialogPrompt("Successfully dumped all of the online files!", "OK");
    else showDialogPrompt("Failed to dump the online files...", "OK");
}

void cleanDumpingProcess() {
    WHBLogPrint("Cleaning up after dumping...");
    WHBLogFreetypeDraw();
    sleep_for(200ms);
    if (copyBuffer != nullptr) free(copyBuffer);
    copyBuffer = nullptr;
    unmountUSBDrive();
    unmountSD();
    if (isDiscMounted() && !loadTitles(true)) {
        WHBLogPrint("Error while reloading titles after disc dumping...");
        WHBLogFreetypeDraw();
        sleep_for(2s);
    }
    unmountDisc();
}