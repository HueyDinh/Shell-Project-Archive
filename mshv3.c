#include <stdio.h> //printf, fgets
#include <stdlib.h> //exit
#include <stdbool.h> // boolean type support
#include <string.h> // strchar
#include <unistd.h> // pipes
#include <sys/types.h> // wait
#include <sys/wait.h> // wait
#include <sys/stat.h> // open/close
#include <fcntl.h> // open/close


// Macro for checking delimiter in state machine
#define DELIMITER_SIMPLE (" \t\n") // Note that null is implicitly a delimiter, just not visible in this
#define DELIMITER_REIDR (" \t\n<>") // We need two sets of delimiter to control which token will be grabbed

// Buffer Size Parameters
enum {
    INBUFFSZ = 256,
    TOKBUFFSZ = 64,
    EXECUNITSZ = 64,
    PIPESZ = 32,
    SEQSZ = 16
};

// Enum for token grabbing state machine
typedef enum States States;
enum States {
    INWORD,
    INSTRING,
    STANDBY,
};

// Custom Union Type For General Redirection w/ Either Path or File Descrptior
typedef union RedirTarget RedirTarget;
union RedirTarget {
    char *path;
    int FD;
};


// Data Structure that Contains Redirection Info
typedef struct RedirInfo RedirInfo; 
struct RedirInfo {

    enum {
        REDIRIN_NONE,
        REDIRIN_DEFAULT,
        REDIRIN_PIPE,
    } redirInMode;

    RedirTarget redirInPath;
    
    enum {
        REDIROUT_NONE,
        REDIROUT_TRUNC,
        REDIROUT_APPEND,
        REDIROUT_PIPE,
    } redirOutMode;

    RedirTarget redirOutPath;

};

// Data Structure that Contains Execution Info For 1 Pipe Segment (Token Arguments & Redirections)
typedef struct ExecInfo ExecInfo;
struct ExecInfo {
    char *_strToParse;
    char **tokens;
    RedirInfo redirData;
};

// Data Structure that Contains Execution Info For 1 Command Sequence (i.e., 1 Pipe from start to end)
typedef struct PipeInfo PipeInfo;
struct PipeInfo {
    char *_strToParse;
    ExecInfo *execInfoArray;
    int numPipeSeg;
    bool wait;
};

// Data Structure for Token Parsing Return
typedef struct Token Token;
struct Token {
    char *tokenPtr;
    char *nextPtr;
    char delimChar;
};

// Parsing
int parsecmd(char *, PipeInfo[], ExecInfo[], char *[]);
int parseSeqSeg(char *, PipeInfo[]);
ExecInfo * parsePipeSeg(PipeInfo *, ExecInfo[]);
char **parseExecInfo(ExecInfo *, char *[]);
int parseRedir(ExecInfo *);
char** parseToken(ExecInfo *, char *[]);
Token getNextToken(char *, char *);
// Pipe Setup and Executions
int runpipe(PipeInfo *);
int connectpipe(PipeInfo *, int[]);
void cleanuppipe(int[], int, int);
int forksetupexec(ExecInfo *, int[]);
bool redirsetup(RedirInfo *, int[]);
// Internal Command Handling;
bool builtIn(ExecInfo*);
void cd(char *[]);

int
main(void) {
    // Declaration

    //String Input Buffer
    char inbuff[INBUFFSZ];
    //Token Buffer
    char *tokbuff[TOKBUFFSZ];
    //Array Containing All Possible Execution Unit's Data (All segments of all pipes in all sequence)
    ExecInfo cmdExecData[EXECUNITSZ];
    //Array Containing All Pipes' Metadata
    PipeInfo cmdPipeData[SEQSZ];

    // Variables
    char *fgetsStatus;
    int numSeq, seqIndex, pipeSetupStatus;


    // Initialization
    pipeSetupStatus = 0;
    fgetsStatus = NULL;
    numSeq = 0;

    // Superloop
    while (true) {

        // Print Out Prompt
        printf("> ");
        fflush(stdout);

        // Get Input
        fgetsStatus = fgets(inbuff, INBUFFSZ, stdin);

        // Read Failure and EOL guard
        if (fgetsStatus == NULL) { // Read error or EOF
            
            if (feof(stdin)) {
                printf("\n");
                exit(0);
            } else {
                continue;
            }// EOF Exit

        } else {
            // Sanitize the newline at the end
            inbuff[strlen(inbuff) - 1] = '\0';
            // Parse Input (More on this later)
            numSeq = parsecmd(inbuff, cmdPipeData, cmdExecData, tokbuff);

            if (numSeq > 0) {
                // Run each sequence 1 after another; Error in 1 sequence that is not pipe setup error will not prevent other sequence from being carried out
                for (seqIndex = 0; seqIndex < numSeq; seqIndex++) {
                    pipeSetupStatus = runpipe(&cmdPipeData[seqIndex]);

                    if (pipeSetupStatus < 0) {
                        fprintf(stderr, "ERROR: Failed to Setup Sequence Number %d\n", seqIndex);
                        break;
                    }
                }
            }
        }
    }
}

// Idea: Pass 1 = Divide input into one string for each seqeunce (i.e., pipeline)
// Pass 2 = For each sequence string, further break down into pipe components' string (argv[] tokens + redirections) (and also determine if the pipeline is backgrounded or not)
// Pass 3 = For each pipe components (I call each component an execution unit), parse the redirection and then parse the argv[] tokens (redirection desscriptions are always AFTER argv[] token description, hence the string will be shortened after redirection parsing)
int
parsecmd(char *inbuff, PipeInfo cmdPipeData[], ExecInfo cmdExecData[], char *tokbuff[]) {

    // Local Var.
    int numSeq, pipeIndex;
    ExecInfo *cmdExecDataHEAD;
    char **tokbuffHEAD;

    // Initialize
    numSeq = 0;
    cmdExecDataHEAD = cmdExecData;
    tokbuffHEAD = tokbuff;

    // Pass 1: Sequence Segmentation Into Pipes
    numSeq = parseSeqSeg(inbuff, cmdPipeData);

    // Pass 2: Pipe Segmentation Into Execution Units
    for (pipeIndex = 0; pipeIndex < numSeq; pipeIndex++) {

        cmdExecDataHEAD = parsePipeSeg(&cmdPipeData[pipeIndex], cmdExecDataHEAD);

        if (cmdExecDataHEAD == NULL) {
            fprintf(stderr, "ERROR: Failed to Parse Pipe Sequence Number %d\n", pipeIndex);
            return -1;
        }
    }

    // Pass 3: Execution Unit Processing (Redirection & Token Parsing)
    cmdExecDataHEAD->_strToParse = NULL; // Termination conditions

    for (cmdExecDataHEAD = cmdExecData; cmdExecDataHEAD->_strToParse != NULL; cmdExecDataHEAD++) {

        tokbuffHEAD = parseExecInfo(cmdExecDataHEAD, tokbuffHEAD);

        if (tokbuffHEAD == NULL) {
            fprintf(stderr, "ERROR: Failed to Parse Execution Sequence Number %ld\n", cmdExecDataHEAD - cmdExecData);
            return -2;
        }
    }
    return numSeq;
}

int
parseSeqSeg(char *inbuff, PipeInfo cmdPipeData[]) {
    // Local Var.
    int counter;
    char *tok;

    // Initialize
    counter = 0;

    for (tok = strtok(inbuff,";"); tok != NULL; tok = strtok(NULL,";")) { // Take priority over " " wrapping (different from bash implementation)
        cmdPipeData[counter]._strToParse = tok; // Store string head in pipe data struct for further mutation down the line
        cmdPipeData[counter].wait = true; // non-backgrounding by default
        counter++;
    }

    return counter; // Return number of execution sequence
}

ExecInfo *
parsePipeSeg(PipeInfo *currentPipeInfo, ExecInfo cmdExecDataHEAD[]) {
    // Local Var.
    int counter;
    int len;
    char *tok;

    // Initialize
    counter = 0;
    len = strlen(currentPipeInfo->_strToParse);
    currentPipeInfo->execInfoArray = cmdExecDataHEAD;

    if (len > 0 && (currentPipeInfo->_strToParse[len - 1] == '&')) { // Is last char &? (is there even a last char?)
        currentPipeInfo->wait = false;
        currentPipeInfo->_strToParse[len - 1] = '\0'; // Delete the & to ensure uniform string format
    }

    for (tok = strtok(currentPipeInfo->_strToParse,"|"); tok != NULL; tok = strtok(NULL,"|")) { // Similar to ; for sequence segmentation. This take priority over " " wrapping
        cmdExecDataHEAD[counter]._strToParse = tok;
        counter++;
    }

    currentPipeInfo->numPipeSeg = counter; // Keep track of number of pipe segment so we can allocate all pipes' FD at once later

    return cmdExecDataHEAD + counter; // counter also needed to keep track of which sub-section of the execution data structure array belongs to which pipe (no null type separation)

}

char **
parseExecInfo(ExecInfo *currentExecInfo, char *tokbuffHEAD[]) {
    // Local Var.
    int redirParseStatus;
    char **newTokBuffHEAD;

    // Initialize
    redirParseStatus = 0;
    newTokBuffHEAD = NULL;

    redirParseStatus = parseRedir(currentExecInfo); // Parse exec info first (which will put a null char between the argv[] tokens and redirection description)

    if (redirParseStatus == 0) {
        
        return parseToken(currentExecInfo, tokbuffHEAD); // If parsing exec info is successful, then start to parse string tokens

    } else {
        return NULL;
    }


}

int
parseRedir(ExecInfo *currentExecInfo) {

    // Local Var.
    int charIndex, len;
    bool skip;
    Token tok;

    //Initialize
    skip = false; // Temporary solution to deal with <> enclosed in ""
    len = strlen(currentExecInfo->_strToParse);
    currentExecInfo->redirData.redirInMode = REDIRIN_NONE; // No redirection by default
    currentExecInfo->redirData.redirOutMode = REDIROUT_NONE;

    for (charIndex = 0; charIndex < len; charIndex++) {
        
        switch (currentExecInfo->_strToParse[charIndex])
        {
        case '>':
            if (skip) continue;
            currentExecInfo->_strToParse[charIndex] = '\0';
            if (currentExecInfo->_strToParse[charIndex + 1] == '>') {
                currentExecInfo->_strToParse[charIndex + 1] = '\0';
                currentExecInfo->redirData.redirOutMode = REDIROUT_APPEND;
                charIndex++;
            } else {
                currentExecInfo->redirData.redirOutMode = REDIROUT_TRUNC;
            }
            currentExecInfo->redirData.redirOutPath.path = &currentExecInfo->_strToParse[charIndex + 1]; // Temp. storage of string head. The true path token will be fetched later
            break;

        case '<':
            if (skip) continue;
            currentExecInfo->_strToParse[charIndex] = '\0';
            currentExecInfo->redirData.redirInMode = REDIRIN_DEFAULT;
            currentExecInfo->redirData.redirInPath.path = &currentExecInfo->_strToParse[charIndex + 1];
            break;
        
        case '"':
            skip = !skip;
            break;

        default:
            //
            break;
        }


    }
    // Checking if parsing was successful
    if (currentExecInfo->redirData.redirInMode != REDIRIN_NONE) {
        tok = getNextToken(currentExecInfo->redirData.redirInPath.path, DELIMITER_REIDR);

        if (tok.tokenPtr != NULL && tok.nextPtr == NULL) { // Unresolved token
            fprintf(stderr,"ERROR: Failure to tokenize string\n");
            return -1;
        } else if (tok.tokenPtr == NULL && tok.nextPtr == NULL) { // No token found
            fprintf(stderr,"ERROR: Destination token expected but not found\n");
            return -2;
        } else {
            currentExecInfo->redirData.redirInPath.path = tok.tokenPtr;
        }
    }

    if (currentExecInfo->redirData.redirOutMode != REDIROUT_NONE) {
        tok = getNextToken(currentExecInfo->redirData.redirOutPath.path, DELIMITER_REIDR);

        if (tok.tokenPtr != NULL && tok.nextPtr == NULL) {
            fprintf(stderr,"ERROR: Failure to tokenize string\n");
            return -1;
        } else if (tok.tokenPtr == NULL && tok.nextPtr == NULL) {
            fprintf(stderr,"ERROR: Destination token expected but not found\n");
            return -2;
        } else {
            currentExecInfo->redirData.redirOutPath.path = tok.tokenPtr;
        }
    }
    return 0;

}

char**
parseToken(ExecInfo *currentExecInfo, char *tokbuffHEAD[]) {
    // Local Var.
    Token tok;
    char * targetString;
    int counter;

    // Initialize
    tok.nextPtr = NULL;
    targetString = currentExecInfo->_strToParse;
    currentExecInfo->tokens = tokbuffHEAD;
    counter = 0;

    while (true) { // fetch token until no more tokens can be found
        tok = getNextToken(targetString, DELIMITER_SIMPLE);
        if (tok.tokenPtr != NULL && tok.nextPtr == NULL) {
            fprintf(stderr,"ERROR: Failure to Tokenize String %s\n", tok.tokenPtr);
            return NULL;
        } else if (tok.tokenPtr == NULL && tok.nextPtr == NULL) {
            tokbuffHEAD[counter] = NULL;
            break;
        } else {
            tokbuffHEAD[counter++] = tok.tokenPtr;
            targetString = tok.nextPtr;
        }
    }

    return tokbuffHEAD + counter + 1;

}

Token // Token data structure = 2 string ptr (token head and next string) and the delimiter char. Previous implementation too monolithic. This version only fetch 1 token at a time
getNextToken(char *targetString, char *CHARSET) {

    // Local Var.
    States state;
    int len;
    int charIndex;
    bool foundToken;
    char tempbuff[INBUFFSZ];
    char *subTokBegin;
    Token returnStruct;

    // Initialize
    state = STANDBY;
    foundToken = false;
    tempbuff[0] = '\0';
    subTokBegin = NULL;
    returnStruct.tokenPtr = NULL;
    returnStruct.nextPtr = NULL;
    returnStruct.delimChar = '\0';
    len = strlen(targetString);
    //

    for (charIndex = 0; charIndex <= len; charIndex++) {

        switch (state)
        {
        case STANDBY:

            if (targetString[charIndex] == '"') {
                state = INSTRING;
                targetString[charIndex] = '\0';
                subTokBegin = &targetString[charIndex+1];

                if (!foundToken) {
                    returnStruct.tokenPtr = subTokBegin;
                    foundToken = true;
                }

            } else if (strchr(CHARSET, targetString[charIndex]) != NULL){
                continue;
            } else {
                state = INWORD;
                subTokBegin = &targetString[charIndex];

                if (!foundToken) {
                    returnStruct.tokenPtr = subTokBegin;
                    foundToken = true;
                }

            }
            break;

        case INWORD:

            if (targetString[charIndex] == '"') {
                state = INSTRING;
                targetString[charIndex] = '\0';
                strcat(tempbuff, subTokBegin);
                subTokBegin = &targetString[charIndex+1];

            } else if (strchr(CHARSET, targetString[charIndex]) != NULL){
                returnStruct.delimChar = targetString[charIndex];
                targetString[charIndex] = '\0';
                strcat(tempbuff, subTokBegin);
                strcpy(returnStruct.tokenPtr, tempbuff);
                returnStruct.nextPtr = (returnStruct.delimChar == '\0')? &targetString[charIndex] : &targetString[charIndex + 1];
                return returnStruct;
            }
            break;

        case INSTRING:

            if (targetString[charIndex] == '"') {

                targetString[charIndex] = '\0';
                strcat(tempbuff, subTokBegin);
                subTokBegin = &targetString[charIndex+1];

                if (strchr(CHARSET, targetString[charIndex + 1]) != NULL) {
                    returnStruct.delimChar = targetString[charIndex + 1];
                    targetString[charIndex+1] = '\0';
                    strcpy(returnStruct.tokenPtr, tempbuff);
                    returnStruct.nextPtr = (returnStruct.delimChar == '\0')? &targetString[charIndex + 1] : &targetString[charIndex + 2];
                    return returnStruct;
                } else {
                    state = INWORD;
                }
            }
            break;
        
        default:
            break;
        }
    }
    return returnStruct;
}

int // Open a bunch of pipes, override execution unit data structure with the neccessary pipe descriptor and let the fork-exec subroutine setup the fork with the exec data structre
runpipe(PipeInfo *pipeInfo) {
    int pipeFDtmp[pipeInfo->numPipeSeg*2]; // Stores all pip file decriptor
    int childPIDArray[pipeInfo->numPipeSeg]; // Stores list of all PID that the parrent wait for
    int connectStatus, processIndex;

    connectStatus = connectpipe(pipeInfo, pipeFDtmp); // Manipulate exec data structures of pipe segment to embed file descriptor
    if (connectStatus < 0) {
        return connectStatus;
    }

    for (processIndex = 0; processIndex < pipeInfo->numPipeSeg; processIndex++) {
        childPIDArray[processIndex] = forksetupexec(pipeInfo->execInfoArray + processIndex, pipeFDtmp);
    }
    cleanuppipe(pipeFDtmp, -1, -1); // Clean-up in parrent (close all pipes' FD after the forks are done). -1 because file descriptors cannot be negative (any impossible file descriptor works)

    if (pipeInfo->wait) {
        for (processIndex = 0; processIndex < pipeInfo->numPipeSeg; processIndex++) {
            if (childPIDArray[processIndex] != 0) { // 0 PID returned means the command is built-in (not to be confused with fork() returning 0)
                waitpid(childPIDArray[processIndex], NULL, 0);
            }
        }
    }
    return 0;
}

int // Return number of pipes FD opened or negative on error
connectpipe(PipeInfo *pipeInfo, int pipeFDtmp[]) {

    int execIndex, numExecLink, pipeCreationStatus;

    numExecLink = pipeInfo->numPipeSeg;

    for (execIndex = 0; execIndex < numExecLink - 1; execIndex++) {
        pipeInfo->execInfoArray[execIndex].redirData.redirOutMode = REDIROUT_PIPE;
        pipeInfo->execInfoArray[execIndex + 1].redirData.redirInMode = REDIRIN_PIPE;

        pipeCreationStatus = pipe(pipeFDtmp + 2*execIndex);

        if (pipeCreationStatus < 0) {
            return -1;
        }

        pipeInfo->execInfoArray[execIndex].redirData.redirOutPath.FD = pipeFDtmp[2*execIndex + 1];
        pipeInfo->execInfoArray[execIndex + 1].redirData.redirInPath.FD = pipeFDtmp[2*execIndex];

    }
    pipeFDtmp[2*execIndex + 2] = -1; //cleanuppipe() uses negative PID to determine the end of the FD array (so that I don't have to pass an int with it everywhere)
    return 2*execIndex;
}

void // Allow 1 FID to be ignored during the clean-up (for pipe set-up purpose)
cleanuppipe(int pipeFDtmp[], int excludeFID1, int excludeFID2) {

    int i;

    for (i = 0; pipeFDtmp[i] >= 0; i++) {
        if (pipeFDtmp[i] != excludeFID1 && pipeFDtmp[i] != excludeFID2) {
            close(pipeFDtmp[i]);
        }
    }
}


int
forksetupexec(ExecInfo *execInfo, int cleanupFD[]) {

    int PID;
    bool isBuiltIn;

    isBuiltIn = builtIn(execInfo); // Intercept and carry out built in command first

    if (isBuiltIn) return 0;
    
    PID = fork();
    if (PID < 0) {
        perror("fork");
        exit(1);
    } else if (PID == 0) {
        
        if (!redirsetup(&execInfo->redirData, cleanupFD)) {
            fprintf(stderr, "ERROR: Redirection setup failed\n");
            exit(1);
        }

        execvp(execInfo->tokens[0], execInfo->tokens);
        perror("exec");
        exit(2);
    }
    return PID;
}

bool
redirsetup(RedirInfo *redirData, int cleanupFD[]) {
    int FID, exclude1, exclude2;

    exclude1 = -1;
    exclude2 = -1;

    switch (redirData->redirInMode)
    {
    case REDIRIN_NONE:
        break;
    
    case REDIRIN_DEFAULT:
        if ((FID = open(redirData->redirInPath.path, O_RDONLY)) >= 0 ) {
            close(0);
            dup(FID);
        } else {
            perror("open");
            return false;
        }
        break;

    case REDIRIN_PIPE:
        close(0);
        dup(redirData->redirInPath.FD);
        exclude1 = redirData->redirInPath.FD;
        break;
    
    default:
        fprintf(stderr, "ERROR: Unrecognized Redirection Mode");
        return false;
    }

    switch (redirData->redirOutMode)
    {
    case REDIROUT_NONE:
        break;
    
    case REDIROUT_APPEND:
        if ((FID = open(redirData->redirOutPath.path, O_APPEND|O_CREAT|O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH)) >= 0 ) {
            close(1);
            dup(FID);
        } else {
            perror("open");
            return false;
        }
        break;

    case REDIROUT_TRUNC:
        if ((FID = open(redirData->redirOutPath.path, O_TRUNC|O_CREAT|O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH)) >= 0 ) {
            close(1);
            dup(FID);
        } else {
            perror("open");
            return false;
        }
        break;

    case REDIROUT_PIPE:
        close(1);
        dup(redirData->redirOutPath.FD);
        exclude2 = redirData->redirOutPath.FD;
        break;
    
    default:
        fprintf(stderr, "ERROR: Unrecognized Redirection Mode");
        return false;
    }

    cleanuppipe(cleanupFD, exclude1, exclude2); // Close everything but the write FD
    return true;

}

bool
builtIn(ExecInfo* execInfo) {

    if (execInfo->tokens[0] == NULL) {
        return true;
    }

    if (strcmp(execInfo->tokens[0], "q") == 0) {
        exit(0);
    }

    if (strcmp(execInfo->tokens[0], "quit") == 0) {
        exit(0);
    }

    if (strcmp(execInfo->tokens[0], "cd") == 0) {
        cd(execInfo->tokens);
        return true;
    }

    return false;

}

void
cd(char *argv[]) {

    int counter;

    for (counter = 0; (argv[counter] != NULL); counter++);

    if (counter != 2) {
        fprintf(stderr, "Usage: cd DIR\n");
    } else {
        if (chdir(argv[1]) < 0) {
            perror("cd");
        }
    }

}
