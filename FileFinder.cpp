#include <iostream>
#include <mutex>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace std;

const int FILENAME_LENGTH = 255; // Max filename length in Linux
const int PATHNAME_LENGTH = 4096; // Max pathname length in Linux
const int PIPE_CAPACITY = 4096; // Pipe capacity in old versions of Linux
// PIPE_CAPACITY is length of interrupted statement after searching directories for files and text

// Class used to assign serial numbers to all processes, and to track what they're doing
// Global variable is used so that *processList is accessible during signals, specifically for kill and quit commands
class Processes {
    public:
        struct ProcessData {
            int pid;
            int searchFlag; // 0 is searching for files, 1 is searching for text within files
            bool isRecursive;
        };
        static const int MAX_PROCESSES = 10;
        
        void initialize();
        int addProcess(const char *searchTerm, const char *fileExtension, const bool isRecursive, const int searchFlag);
        bool removeProcess(const int serialNumber);
        bool removeSelf();
        void startWriting(int serialNumber);
        void getSerialNumbers(int intArray[MAX_PROCESSES]);
        ProcessData getProcessData(const int serialNumber, char emptySearchTerm[], char emptyFileExtension[]); // empty strings >= 255 chars
        int getPID(int serialNumber);
        bool isProcessWriting(int serialNumber);
        void destroyChild();
        
    private:
        mutex mtx;
        struct {
            bool active;
            int pid;
            char searchTerm[FILENAME_LENGTH];
            char fileExtension[FILENAME_LENGTH];
            int searchFlag;
            bool isRecursive;
            bool isWriting;
        } processes[MAX_PROCESSES];
} *processList = (Processes*)mmap(NULL, sizeof(Processes), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

// Class used to store pointers to all DIR pointers in searchDirectories()
// searchDirectories() is recurssive and killing a child process with open directories causes massive memory leak
// Global variable directoryList is used to close all directories after receiving a kill signal from parent process
class Directories {
    public:
        ~Directories(); 
        void initialize() { root = NULL; }
        void addDIR(DIR **dir); 
        void removeDIR(DIR **dir); 
        void closeAllDIRAndDestroy();
    private:
        struct node {
            DIR **dir;
            node *next;
        } *root;
} directoryList;

enum Command_Type { FIND, LIST, KILL, QUIT, INVALID };
typedef struct {
    Command_Type commandType;

    // Command_Type FIND
    int searchFlag;
    char searchText[FILENAME_LENGTH];
    char fileExtension[FILENAME_LENGTH]; 
    bool searchSubDir;

    // Command_Type KILL
    int id;
} Command;

bool parseInput(char *userInput, int inputSize, char *parsedInput[]);
Command parseCommand(char *arg[]);
bool issueCommand(const Command &command, int *pipeSize, bool *stdinOverwritten);

bool findCommand(const Command &command, int *pipeSize, bool *stdinOverwritten);
void fillPrintMessage(const Command &command, char *message, int *stringLength, int serialNumber); 
void searchDirectories(const Command &command, char *message, int *messageLength, char *directory, bool *foundSomething);

void listCommand();
void killCommand(int killSerialNumber, bool writeOutput);
bool quitCommand();

void fillFilePath(char *directory, char *filename, char *filePath);
bool isDirectory(const char *directory);
bool isRegFile(const char *filePath);
bool hasCorrectExtension(const char *filename, const char *fileExtension);
bool isTextInFile(const char *filePath, const char *searchText);
bool isPreviousDir(char *filename) { return (strcmp(filename, "..") == 0); }
bool isCurrentDir(char *filename) { return (strcmp(filename, ".") == 0); }
void appendString(char *mainString, const char *stringToAppend, int *mainStringSize);
void fillTimeEllapsedString(float timeInSeconds, char str[13]);
void waitRunningProcesses(bool shouldHang); 

const int PARENT_ID = getpid();
int fd[2]; // Global variable used as pipe to switch STDIN with
void stdinOverwrite(int i) { dup2(fd[0], STDIN_FILENO); }

bool childIsWriting; // Global variable used during signal calls to indicate where child process is (for use with kill and quit commands)
void childKill(int i);
mutex mtx;

int main() 
{
    pipe(fd);
    signal(SIGUSR1, stdinOverwrite);
    int save_stdin = dup(STDIN_FILENO);

    childIsWriting = false;
    signal(SIGUSR2, childKill);

    processList->initialize();
    directoryList.initialize();

    int *pipeSize = (int*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *pipeSize = 0;
    
    bool *stdinOverwritten = (bool*)mmap(NULL, sizeof(bool), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *stdinOverwritten = false;

    char userInput[PIPE_CAPACITY];
    bool loop = true;
    while (loop) {
        waitRunningProcesses(false);

        printf("\033[1;94;49mfindstuff\033[0m$ ");
        fflush(stdout);
        read(STDIN_FILENO, userInput, PIPE_CAPACITY);

        if (*stdinOverwritten) {
            userInput[*pipeSize] = 0;
            printf("%s", userInput);
            dup2(save_stdin, STDIN_FILENO);
            *stdinOverwritten = false;
        }
        else
        {
            char *arg[4] = {NULL, NULL, NULL, NULL};
            int inputLength = 1;
            for (; userInput[inputLength - 1] != '\n'; inputLength++);
            if (parseInput(userInput, inputLength, arg))
            {
                Command command = parseCommand(arg);
                loop = issueCommand(command, pipeSize, stdinOverwritten);
            }
        }
    }

    if (getpid() == PARENT_ID) // Avoids child processes interfering as they exit loop and close
    {
        munmap(pipeSize, sizeof(int));
        munmap(stdinOverwritten, sizeof(bool));
        //munmap(processList, sizeof(Processes)); // Segfaults for some reason
        close(fd[0]);
        close(fd[1]);
        waitRunningProcesses(true);
    }
    return 0;
}

bool parseInput(char *userInput, int inputSize, char *parsedInput[])
{
    int currentArg = 0;
    int argStart = 0, argEnd = 0;
    bool isInQuotes = false;
    for (int i = 0; i < inputSize; i++)
        if (currentArg == 1 && argEnd == i && userInput[i] == '\"')
            isInQuotes = true;
        else if (isInQuotes && userInput[i] == '\"')
            isInQuotes = false;
        else if ((!isInQuotes && userInput[i] == ' ') || userInput[i] == '\n')
        {
            if (currentArg > 3)
            {
                for (int i = 0; i < 4; i++)
                    delete[] parsedInput[i];
                printf("ERROR. Too many arguments.\nExpected no more than 4.\n");
                return false;
            }
            argStart = argEnd;
            argEnd = i;
            int wordSize = argEnd - argStart;
            parsedInput[currentArg] = new char[wordSize + 1];
            memcpy(parsedInput[currentArg], userInput + argStart, wordSize);
            parsedInput[currentArg][wordSize] = 0;
            argEnd++;
            currentArg++;
        }
    return true;
}

Command parseCommand(char *arg[])
{
    Command command;
    if (strcmp(arg[0], "find") == 0) 
    {
        command.commandType = Command_Type::FIND;
        bool dashSSet = false;
        bool extSet = false;
        if (arg[1][0] == '"' && arg[1][strlen(arg[1]) - 1] == '"')
        {
            command.searchFlag = 1;
            arg[1][strlen(arg[1]) - 1] = 0;
            strcpy(command.searchText, arg[1] + 1);
            for (int i = 2; i < 4; i++)
            {
                if (arg[i] != NULL)
                {
                    if (strcmp(arg[i], "-s") == 0)
                    {
                        command.searchSubDir = true;
                        dashSSet = true;
                    }
                    else if (arg[i][0] == '-' && arg[i][1] == 'f' && arg[i][2] == ':')
                    {
                        strcpy(command.fileExtension, arg[i] + 3);
                        extSet = true;
                    }
                    else
                    {
                        printf("ERROR. Argument %s not recognized. Expected -s or -f: for text find command.\n", arg[i]);
                        command.commandType = Command_Type::INVALID;
                        break;
                    }
                }
            }
            if (!extSet)
                command.fileExtension[0] = 0;
        }
        else
        {
            command.searchFlag = 0;
            command.fileExtension[0] = 0;
            strcpy(command.searchText, arg[1]);
            if (arg[2] != NULL)
            {
                if (strcmp(arg[2], "-s") == 0)
                {
                    command.searchSubDir = true;
                    dashSSet = true;
                }
                else
                {
                    printf("ERROR. Argument %s not recognized. Expected -s for file find command.\n", arg[2]);
                    command.commandType = Command_Type::INVALID;
                }
            }
        }
        if (!dashSSet)
            command.searchSubDir = false;
    }
    else if (strcmp(arg[0], "list") == 0) 
        command.commandType = Command_Type::LIST;
    else if (strcmp(arg[0], "kill") == 0) 
    {
        command.commandType = Command_Type::KILL;
        if (arg[1] == NULL || strlen(arg[1]) > 1 || arg[1][0] < 48 || arg[1][0] > 57) // 48 = '0' and 57 = '9'
        {
            printf("ERROR. Argument %s not recognized. Expected integer value between 0-9 for kill command.\n", arg[1]);
            command.commandType = Command_Type::INVALID;
        }
        else
            command.id = arg[1][0] - 48;
    }
    else if (strcmp(arg[0], "quit") == 0 || strcmp(arg[0], "q") == 0) 
        command.commandType = Command_Type::QUIT;
    else    
    {
        command.commandType = Command_Type::INVALID;
        printf("ERROR. Argument %s not recognized.\n", arg[0]);
    }

    for (int i = 0; i < 4; i++)
    {
        delete[] arg[i];
        arg[i] = NULL;
    }
    return command;

}

bool issueCommand(const Command &command, int *pipeSize, bool *stdinOverwritten)
{
    if (command.commandType == Command_Type::FIND)
    {
        if (fork() == 0) // Parent returns true and continues to loop
            return findCommand(command, pipeSize, stdinOverwritten); // Child returns false and exits loop to close program
    }
    else if (command.commandType == Command_Type::LIST)
        listCommand();
    else if (command.commandType == Command_Type::KILL)
        killCommand(command.id, true);
    else if (command.commandType == Command_Type::QUIT)
        return quitCommand(); // Returns false to exit loop
    return true;
}

bool findCommand(const Command &command, int *pipeSize, bool *stdinOverwritten)
{
    close(fd[0]);
    char printMessage[PIPE_CAPACITY];
    int len = 0;
    int *stringLength = &len;
    int serialNumber = processList->addProcess(command.searchText, command.fileExtension, command.searchSubDir, command.searchFlag);

    fillPrintMessage(command, printMessage, stringLength, serialNumber);
    
    while (*stdinOverwritten); // Wait until pipe contents have been read before writing more
    processList->startWriting(serialNumber);
    mtx.lock();

    childIsWriting = true; // Prevents kill command from killing this process, as process is finished searching and killing after this point affects output
    *pipeSize += len;
    kill(PARENT_ID, SIGUSR1); // Redirects stdin to pipe
    *stdinOverwritten = true; 
    write(fd[1], printMessage, len);

    mtx.unlock();
    while (*stdinOverwritten);
    *pipeSize -= len;
    close(fd[1]);
    return false;
}

void fillPrintMessage(const Command &command, char *message, int *stringLength, int serialNumber)
{
    appendString(message, "Interrupt: Process ", stringLength);
    if (serialNumber == -1)
    {
        appendString(message, "cannot be completed.\nCannot search for ", stringLength);
        if (command.searchFlag == 0)
            appendString(message, "file ", stringLength);
        else
            appendString(message, "instance of \"", stringLength);
        appendString(message, command.searchText, stringLength);
        if (command.searchFlag == 1)
            appendString(message, "\"\n", stringLength);
        else
            appendString(message, "\n", stringLength);
        appendString(message, "Maximum 10 processes at a time.\nPlease try again later.\n", stringLength);
        return;
    }

    string serialString = to_string(serialNumber);
    char const *serialCharArr = serialString.c_str();
    appendString(message, serialCharArr, stringLength);
    appendString(message, " ", stringLength);
    
    bool foundSomething = false;
    char directory[PATHNAME_LENGTH];
    getcwd(directory, PATHNAME_LENGTH);

    clock_t start = clock();
    searchDirectories(command, message, stringLength, directory, &foundSomething);
    clock_t end = clock();
    double timeElapsed = (double)(end - start) / CLOCKS_PER_SEC;

    if (!foundSomething)
    {

        appendString(message, "completed.\nUnable to find ", stringLength);
        if (command.searchFlag == 0)
            appendString(message, "file ", stringLength);
        else
            appendString(message, "instance of \"", stringLength);
        appendString(message, command.searchText, stringLength);
        if (command.searchFlag == 1)
            appendString(message, "\".\n", stringLength);
        else
            appendString(message, ".\n", stringLength);
    }
    char elapsedString[13];
    fillTimeEllapsedString(timeElapsed, elapsedString);
    appendString(message, "Time elapsed: ", stringLength);
    appendString(message, elapsedString, stringLength);
    appendString(message, ".\n", stringLength);
}

/*
if (command.searchFlag == 0) then find function will search for filenames that match searchText
if (command.searchFlag == 1) then find function will search for files that contain an instance of searchText
*/
void searchDirectories(const Command &command, char *message, int *messageLength, char *directory, bool *foundSomething)
{
    DIR *dir;
    struct dirent *entry;
    dir = opendir(directory);
    if (dir == NULL) 
    {
        appendString(message, "invalid directory ", messageLength);
        appendString(message, directory, messageLength);
        return;
    }
    directoryList.addDIR(&dir);

    while ((entry = readdir(dir)) != NULL) 
    {
        if (!isPreviousDir(entry->d_name) && !isCurrentDir(entry->d_name))
        {
            char filePath[PATHNAME_LENGTH];
            fillFilePath(directory, entry->d_name, filePath);

            if (command.searchSubDir && isDirectory(filePath))
                searchDirectories(command, message, messageLength, filePath, foundSomething);

            if (command.searchFlag == 0 && strcmp(entry->d_name, command.searchText) == 0) 
            {
                if (!*foundSomething)
                {
                    *foundSomething = true;
                    appendString(message, "completed.\nFile ", messageLength);
                    appendString(message, command.searchText, messageLength);
                    appendString(message, " found at:\n", messageLength);
                }
                appendString(message, directory, messageLength);
                appendString(message, "\n", messageLength);
            }
            else if (command.searchFlag == 1 && isRegFile(filePath) &&
                    hasCorrectExtension(entry->d_name, command.fileExtension)  &&
                    isTextInFile(filePath, command.searchText))
            {
                if (!*foundSomething)
                {
                    *foundSomething = true;
                    appendString(message, "completed.\nText \"", messageLength);
                    appendString(message, command.searchText, messageLength);
                    appendString(message, "\" found in:\n", messageLength);
                }
                appendString(message, filePath, messageLength);
                appendString(message, "\n", messageLength);
            }
        }
    }
    directoryList.removeDIR(&dir);
    closedir(dir);
    return;
}

void listCommand()
{
    int serialNumbers[Processes::MAX_PROCESSES];
    processList->getSerialNumbers(serialNumbers);

    bool processesAreActive = false;
    char searchTerm[255];
    char fileExtension[255];
    for (int i = 0; serialNumbers[i] != -1 && i < Processes::MAX_PROCESSES; i++)
        if (!(processList->isProcessWriting(serialNumbers[i])))
        {
            processesAreActive = true;
            Processes::ProcessData pd = processList->getProcessData(serialNumbers[i], searchTerm, fileExtension);
            printf("Process %d: searching for ", serialNumbers[i]);
            if (pd.searchFlag == 0)
            {
                printf("file %s ", searchTerm);
                if (pd.isRecursive)
                    printf("recursively.\n");
                else
                    printf("in current directory.\n");
            }
            else
            {
                printf("text \"%s\" ", searchTerm);
                if (pd.isRecursive)
                    printf("recursively ");
                else
                    printf("in current directory ");
                printf("in all ");
                if (fileExtension[0] != 0)
                    printf("%s ", fileExtension);
                printf("files.\n");
            }
        }
    if (!processesAreActive)
        printf("There are no processes currently running.\n");
}

void killCommand(int killSerialNumber, bool writeOutput)
{
    int killPID = processList->getPID(killSerialNumber);
    if (killPID != -1)
    {
        kill(killPID, SIGUSR2);
        int status;
        waitpid(killPID, &status, 0);
        if (writeOutput)
            printf("Process %d has been killed\n", killSerialNumber);
    }
    else
    {
        printf("Process %d could not be killed.\n", killSerialNumber);
        printf("This was either an invalid entry or this process has already finished running.\n");
    }
}

bool quitCommand()
{
    int serialNumbers[Processes::MAX_PROCESSES];
    processList->getSerialNumbers(serialNumbers);
    for (int i = 0; serialNumbers[i] != -1 && i < Processes::MAX_PROCESSES; i++)
        killCommand(serialNumbers[i], false);
    return false;
}

void fillFilePath(char *directory, char *filename, char *filePath) 
{
    strcpy(filePath, directory);
    filePath[strlen(directory)] = '/';
    strcpy(filePath + strlen(directory) + 1, filename);
}

bool isDirectory(const char *filePath)
{
    struct stat sb;
    int check = stat(filePath, &sb);
    if (S_ISDIR(sb.st_mode))
        return true;
    return false;
}

bool isRegFile(const char *filePath)
{
    struct stat sb;
    int check = stat(filePath, &sb);
    if (S_ISREG(sb.st_mode))
        return true;
    return false;
}

bool hasCorrectExtension(const char *filename, const char *fileExtension) {
    int extSize = strlen(fileExtension);
    if (extSize == 0)
        return true;

    int nameSize = strlen(filename);
    int sizeDiff = nameSize - extSize;

    int i = nameSize - 1;
    for (int j = i - sizeDiff; j >= 0; i--, j--)
        if (filename[i] != fileExtension[j])
            return false;
    if (filename[i] != '.')
        return false;

    return true;
}

bool isTextInFile(const char *filePath, const char *searchText)
{
    FILE *file = fopen(filePath, "r");
    if (file == NULL) 
    {
        printf("ERROR: could not open file: %s\n", filePath);
        return false;
    }
    fseek(file, 0, SEEK_END);
    long int size = ftell(file);
    rewind(file);

    char* fileContents = new char[size + 1];
    fread(fileContents, 1, size, file);
    fileContents[size] = 0;
    fclose(file);

    if (strstr(fileContents, searchText)) 
    {
        delete[] fileContents;
        return true;
    }
    delete[] fileContents;
    return false;
}

// For use only with fillPrintMessage() and searchDirectories()
// Assumes mainString is of length global const int PIPE_CAPACITY
void appendString(char *mainString, const char *stringToAppend, int *mainStringSize)
{
    int totalSize = *mainStringSize + strlen(stringToAppend);
    if (totalSize > PIPE_CAPACITY)
    {
        int availableSpace = PIPE_CAPACITY - *mainStringSize;
        if (availableSpace == 0)
            return;
        char newString[availableSpace];
        memcpy(newString, stringToAppend, availableSpace - 1);
        newString[availableSpace - 1] = 0;
        newString[availableSpace - 2] = '\n';
        strcpy(mainString + *mainStringSize, newString);
        *mainStringSize = PIPE_CAPACITY;
    }
    else
    {
        strcpy(mainString + *mainStringSize, stringToAppend);
        *mainStringSize = totalSize;
    }
}

void waitRunningProcesses(bool shouldHang)
{
    int status;
    int activeList[Processes::MAX_PROCESSES];
    processList->getSerialNumbers(activeList);
    for (int i = 0; activeList[i] != -1 && i < Processes::MAX_PROCESSES; i++)
    {
        int pid = processList->getPID(activeList[i]);
        if (shouldHang)
            waitpid(pid, &status, 0);
        else
        {
            if (waitpid(pid, &status, WNOHANG) == pid)
                processList->removeProcess(activeList[i]);
        }
            
    }
}

void fillTimeEllapsedString(float timeInSeconds, char str[13])
{
    const unsigned int SS_IN_HH = 3600;
    const unsigned int SS_IN_MM = 60;
    const unsigned int MS_IN_SS = 1000;
    int unsigned hh = timeInSeconds / SS_IN_HH;
    int unsigned mm = (timeInSeconds - (hh * SS_IN_HH)) / SS_IN_MM;
    int unsigned ss = timeInSeconds - (hh * SS_IN_HH) - (mm * SS_IN_MM);
    int unsigned ms = MS_IN_SS * (timeInSeconds - (int)timeInSeconds);
    str[2] = ':'; str[5] = ':'; str[8] = ':'; str[12] = 0;
    if (hh > 99)
    {
        str[0] = '9'; str[1] = '9'; 
        str[3] = '5'; str[4] = '9';
        str[6] = '5'; str[7] = '9';
        str[9] = '9'; str[10] = '9'; str[11] = '9';
    }   
    else 
    {
        str[0] = ((int)(hh / 10)) + '0';
        str[1] = (hh % 10) + '0'; 
        str[3] = ((int)(mm / 10)) + '0';
        str[4] = (mm % 10) + '0';
        str[6] = ((int)(ss / 10)) + '0';
        str[7] = (ss % 10) + '0';
        str[9] = ((int)(ms / 100)) + '0';
        str[10] = (int)((int)(ms / 10) % 10) + '0';
        str[11] = (ms % 10) + '0';
    }
}

// Child is finished anyway once it's in writing process
void childKill(int i)
{
    if (!childIsWriting)
    {
        directoryList.closeAllDIRAndDestroy();
        processList->removeSelf();
        kill(getpid(), SIGTERM);
    }       
} 

void Processes::initialize()
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        processes[i].active = false;

}

int Processes::addProcess(const char *searchTerm, const char *fileExtension, const bool isRecursive, const int searchFlag)
{
    mtx.lock();
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (!processes[i].active)
        {
            processes[i].active = true;
            processes[i].pid = getpid();
            strcpy(processes[i].searchTerm, searchTerm);
            strcpy(processes[i].fileExtension, fileExtension);
            processes[i].isRecursive = isRecursive;
            processes[i].searchFlag = searchFlag;
            processes[i].isWriting = false;
            mtx.unlock();
            return i;
        }
    }
    mtx.unlock();
    return -1;
}

bool Processes::removeProcess(const int serialNumber) {
    mtx.lock();
    if (processes[serialNumber].active)
    {
        processes[serialNumber].active = false;
        mtx.unlock();
        return true;
    }
    mtx.unlock();
    return false;
}

bool Processes::removeSelf()
{
    int pid = getpid();

    mtx.lock();
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].active && processes[i].pid == pid)
        {
            processes[i].active = false;
            mtx.unlock();
            return true;
        }
    mtx.unlock();
    return false;
}

void Processes::startWriting(int serialNumber)
{
    mtx.lock();
    if (processes[serialNumber].active)
        processes[serialNumber].isWriting = true;
    mtx.unlock();
}

void Processes::getSerialNumbers(int intArray[MAX_PROCESSES])
{
    int finalIndex = 0;
    mtx.lock();
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].active)
        {
            intArray[finalIndex] = i;
            finalIndex++;
        }
    mtx.unlock();
    for (; finalIndex < MAX_PROCESSES; finalIndex++)
        intArray[finalIndex] = -1;
}

Processes::ProcessData Processes::getProcessData(const int serialNumber, char emptySearchTerm[], char emptyFileExtension[])
{
    ProcessData returnData;
    mtx.lock();
    if (processes[serialNumber].active)
    {
        returnData.pid = processes[serialNumber].pid;
        returnData.isRecursive = processes[serialNumber].isRecursive;
        returnData.searchFlag = processes[serialNumber].searchFlag;
        strcpy(emptySearchTerm, processes[serialNumber].searchTerm);
        strcpy(emptyFileExtension, processes[serialNumber].fileExtension);
        mtx.unlock();
    }
    else
    {
        mtx.unlock();
        returnData.pid = -1;
        returnData.isRecursive = false;
        returnData.searchFlag = -1;
        emptySearchTerm[0] = 0;
        emptyFileExtension[0] = 0;
    }
    return returnData;
}

int Processes::getPID(int serialNumber)
{
    mtx.lock();
    if (processes[serialNumber].active)
    {
        int returnValue = processes[serialNumber].pid;
        mtx.unlock();
        return returnValue;
    }
    mtx.unlock();
    return -1;
}

bool Processes::isProcessWriting(int serialNumber)
{
    mtx.lock();
    if (processes[serialNumber].active)
    {
        bool returnValue = processes[serialNumber].isWriting;
        mtx.unlock();
        return returnValue;
    }
    mtx.unlock();
    return false; 
} 

Directories::~Directories()
{
    node *current = root;
    while (current != NULL)
    {
        node *next = current->next;
        delete current;
        current = next;
    }
}

void Directories::addDIR(DIR **dir)
{
    node *newNode = new node;
    newNode->dir = dir;
    newNode->next = NULL;

    if (root == NULL)
        root = newNode;
    else
    {
        node *occupied = root;
        for (; occupied->next != NULL; occupied = occupied->next);
        occupied->next = newNode;
    }
}
  
void Directories::removeDIR(DIR **dir)
{
    if (root->next == NULL)
    {
        if (root->dir == dir)
        {
            delete root;
            root = NULL;
        }
    }
    else
    {
        node *current = root;
        for (; current->next != NULL; current = current->next)
            if (current->next->dir == dir)
            {
                node *nextNext = current->next->next;
                delete current->next;
                current->next = nextNext;
                return;
            }
    }
}

void Directories::closeAllDIRAndDestroy()
{
    node *current = root;
    while (current != NULL)
    {
        node *next = current->next;
        closedir(*(current->dir));
        delete current;
        current = next;
    }
}
