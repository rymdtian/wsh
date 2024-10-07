#include "wsh.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_INPUT_LEN 1024
#define MAX_NUM_TOKENS 128
#define MAX_PATH_LEN 1024
#define INITIAL_LOCAL_VARS_CAPACITY 128
#define INITIAL_HISTORY_CAPACITY 5

const BuiltinCommandInfo BuiltinCommandInfoMap[] = {
    {"exit", executeExitCommand},     {"cd", executeCdCommand},
    {"export", executeExportCommand}, {"local", executeLocalCommand},
    {"vars", executeVarsCommand},     {"history", executeHistoryCommand},
    {"ls", executeLsCommand}};

const int NumBuiltinCommands =
    sizeof(BuiltinCommandInfoMap) / sizeof(BuiltinCommandInfoMap[0]);

const RedirectFlag RedirectFlags[] = {{0, 0},
                                      {O_RDONLY, 0},
                                      {O_WRONLY | O_CREAT | O_TRUNC, 0644},
                                      {O_WRONLY | O_CREAT | O_APPEND, 0644},
                                      {O_WRONLY | O_CREAT | O_TRUNC, 0644},
                                      {O_WRONLY | O_CREAT | O_APPEND, 0644}};

int main(int argc, char **argv) {
  if (argc > 2) {
    fprintf(stderr, "wsh: takes one or no arguments\n");
    return 1;
  }
  Shell *S = initShell();
  int Err = argc == 1 ? runInteractiveMode(S) : runBatchMode(S, argv[1]);
  return -Err;
}

Shell *initShell(void) {
  Shell *S = (Shell *)malloc(sizeof(Shell));
  if (S == NULL) {
    perror("malloc");
    exit(1);
  }
  S->VA = initLocalVariables(INITIAL_LOCAL_VARS_CAPACITY);
  if (S->VA == NULL) {
    fprintf(stderr, "wsh: error initializing\n");
    exit(1);
  }
  if (setenv("PATH", "/bin", 1) < 0) {
    fprintf(stderr, "wsh: error initializing\n");
    exit(1);
  }
  S->Hist = initHistory(INITIAL_HISTORY_CAPACITY);
  if (S->Hist == NULL) {
    fprintf(stderr, "wsh: error initialzing\n");
    exit(1);
  }
  S->Error = 0;
  return S;
}

void freeShell(Shell *S) {
  if (S == NULL)
    return;
  freeLocalVariableArray(S->VA);
  freeHistory(S->Hist);
  free(S);
}

int runInteractiveMode(Shell *S) {
  char Input[MAX_INPUT_LEN];
  for (;;) {
    printf("wsh> ");
    fflush(stdout);
    if (fgets(Input, sizeof(Input), stdin) == NULL) {
      break;
    }
    fflush(stdout);
    Input[strcspn(Input, "\n")] = 0;
    if (strlen(Input) == 0)
      continue;
    Command *Cmd = getCommand(Input, S->VA);
    if (Cmd == NULL)
      continue;
    S->Error = execute(Cmd, S);
    freeCommand(Cmd);
  }
  int Error = S->Error;
  freeShell(S);
  return Error;
}

int runBatchMode(Shell *S, const char *Path) {
  FILE *File = fopen(Path, "r");
  if (File == NULL) {
    perror("fopen");
    exit(1);
  }
  char Buffer[MAX_INPUT_LEN];
  while (fgets(Buffer, sizeof(Buffer), File) != 0) {
    Buffer[strcspn(Buffer, "\n")] = 0;
    if (strlen(Buffer) == 0)
      continue;
    Command *Cmd = getCommand(Buffer, S->VA);
    if (Cmd == NULL)
      continue;
    S->Error = execute(Cmd, S);
    freeCommand(Cmd);
  }
  int Error = S->Error;
  freeShell(S);
  return Error;
}

char *findExecutable(const char *ExeToken) {
  if (ExeToken == NULL)
    return NULL;
  static char ExecutablePath[MAX_PATH_LEN];
  char *Exe = strdup(ExeToken);
  if (Exe == NULL) {
    perror("strdup");
    return NULL;
  }
  if (strchr(Exe, '/') != NULL) {
    if (access(Exe, X_OK) == 0)
      return Exe;
    free(Exe);
    return NULL;
  }
  char *Path = getenv("PATH");
  if (Path == NULL) {
    free(Exe);
    return NULL;
  }
  char *Dir = strtok(Path, ":");
  while (Dir != NULL) {
    snprintf(ExecutablePath, sizeof(ExecutablePath), "%s%s%s", Dir, "/", Exe);
    if (access(ExecutablePath, X_OK) == 0) {
      free(Exe);
      return ExecutablePath;
    }
    Dir = strtok(NULL, ":");
  }
  free(Exe);
  return NULL;
}

int openRedirect(Redirect *R) {
  if (R == NULL || R->File == NULL || R->Mode == RedirectNone)
    return -1;
  int fd =
      open(R->File, RedirectFlags[R->Mode].Flags, RedirectFlags[R->Mode].Mode);
  if (fd == -1)
    fprintf(stderr, "%s: no such file or directory\n", R->File);
  return fd;
}

int redirect(Redirect *R) {
  if (R == NULL || R->File == NULL)
    return 1;
  if (R->Mode == RedirectNone)
    return 1;

  int fd = openRedirect(R);
  if (fd == -1)
    return 0;

  if (R->FD >= 0)
    return dup2(fd, R->FD) != -1;

  switch (R->Mode) {
  case RedirectInput:
    return dup2(fd, STDIN_FILENO) != -1;
  case RedirectOutput:
  case RedirectAppend:
    return dup2(fd, STDOUT_FILENO) != -1;
  case RedirectOuputError:
  case RedirectAppendError:
    return dup2(fd, STDOUT_FILENO) != -1 && dup2(fd, STDERR_FILENO) != 1;
  default:
    return 1;
  }
}

Command *getCommand(char *Input, LocalVariableArray *VA) {
  if (Input == NULL || VA == NULL)
    return NULL;
  Command *Cmd = (Command *)malloc(sizeof(Command));
  if (Cmd == NULL)
    return NULL;
  Cmd->Tokens = NULL;
  Cmd->TokenCount = 0;
  Cmd->Redirection = NULL;
  Cmd->Tokens = (char **)calloc(MAX_NUM_TOKENS, sizeof(char *));
  if (Cmd->Tokens == NULL) {
    free(Cmd);
    return NULL;
  }
  Cmd->Tokens[0] = '\0';
  Cmd->TokenCount = 0;
  char *Token = strtok(Input, " ");
  while (Token != NULL && Cmd->TokenCount < MAX_NUM_TOKENS) {
    Cmd->Tokens[Cmd->TokenCount] = strdup(Token);
    if (Cmd->Tokens[Cmd->TokenCount] == NULL) {
      freeCommand(Cmd);
      return NULL;
    }
    Cmd->TokenCount++;
    Token = strtok(NULL, " ");
  }
  if (Cmd->TokenCount == 0) {
    freeCommand(Cmd);
    return NULL;
  }
  if (Cmd->TokenCount > 1) {
    char *LastToken = strdup(Cmd->Tokens[Cmd->TokenCount - 1]);
    if (LastToken == NULL) {
      perror("strdup");
      freeCommand(Cmd);
      return NULL;
    }
    Cmd->Redirection = (Redirect *)malloc(sizeof(Redirect));
    if (Cmd->Redirection == NULL) {
      freeCommand(Cmd);
      free(LastToken);
      return NULL;
    }
    Cmd->Redirection->File = NULL;
    int RedirectLen;
    Cmd->Redirection->FD = -1;
    if (strstr(LastToken, "&>>")) {
      Cmd->Redirection->Mode = RedirectAppendError;
      RedirectLen = 3;
    } else if (strstr(LastToken, ">>")) {
      Cmd->Redirection->Mode = RedirectAppend;
      RedirectLen = 2;
    } else if (strstr(LastToken, "&>")) {
      Cmd->Redirection->Mode = RedirectOuputError;
      RedirectLen = 2;
    } else if (strchr(LastToken, '>')) {
      Cmd->Redirection->Mode = RedirectOutput;
      RedirectLen = 1;
    } else if (strchr(LastToken, '<')) {
      Cmd->Redirection->Mode = RedirectInput;
      RedirectLen = 1;
    } else {
      Cmd->Redirection->Mode = RedirectNone;
      RedirectLen = 0;
    }
    char *EndPtr;
    if (Cmd->Redirection->Mode != RedirectNone) {
      if (isdigit(LastToken[0])) {
        Cmd->Redirection->FD = (int)strtol(LastToken, &EndPtr, 10);
        Cmd->Redirection->File = strdup(EndPtr + RedirectLen);
        if (Cmd->Redirection->File == NULL) {
          perror("strdup");
          freeCommand(Cmd);
          free(LastToken);
          return NULL;
        }
      } else {
        Cmd->Redirection->File = strdup(LastToken + RedirectLen);
        if (Cmd->Redirection->File == NULL) {
          perror("strdup");
          freeCommand(Cmd);
          free(LastToken);
          return NULL;
        }
      }
    }
    free(LastToken);
  }
  return Cmd->Tokens[0][0] == '#' ? NULL : Cmd;
}

Command *getCommandCopy(Command *Cmd) {
  if (Cmd == NULL)
    return NULL;
  Command *CmdCpy = (Command *)malloc(sizeof(Command));
  if (CmdCpy == NULL)
    return NULL;
  CmdCpy->Tokens = NULL;
  CmdCpy->Redirection = (Redirect *)malloc(sizeof(Redirect));
  if (CmdCpy->Redirection == NULL) {
    freeCommand(CmdCpy);
    return NULL;
  }
  CmdCpy->Redirection->File = NULL;
  if (Cmd->Redirection != NULL) {
    CmdCpy->Redirection->Mode = Cmd->Redirection->Mode;
    CmdCpy->Redirection->File =
        Cmd->Redirection->File == NULL ? NULL : strdup(Cmd->Redirection->File);
  }
  CmdCpy->TokenCount = Cmd->TokenCount;
  CmdCpy->Tokens = (char **)calloc(Cmd->TokenCount, sizeof(char *));
  if (CmdCpy->Tokens == NULL) {
    freeCommand(CmdCpy);
  }
  for (int i = 0; i < Cmd->TokenCount; i++) {
    CmdCpy->Tokens[i] = strdup(Cmd->Tokens[i]);
    if (CmdCpy->Tokens[i] == NULL) {
      freeCommand(CmdCpy);
    }
  }
  return CmdCpy;
}

BuiltinCommandInfo *getBuiltinCommandInfo(Command *Cmd) {
  if (Cmd == NULL)
    return NULL;
  for (int i = 0; i < NumBuiltinCommands; i++)
    if (strcmp(Cmd->Tokens[0], BuiltinCommandInfoMap[i].Name) == 0)
      return (BuiltinCommandInfo *)&BuiltinCommandInfoMap[i];
  return NULL;
}

void freeCommand(Command *Cmd) {
  if (Cmd == NULL)
    return;
  if (Cmd->Tokens) {
    for (int i = 0; i < Cmd->TokenCount; i++)
      if (Cmd->Tokens[i])
        free(Cmd->Tokens[i]);
    free(Cmd->Tokens);
  }
  if (Cmd->Redirection) {
    if (Cmd->Redirection->File)
      free(Cmd->Redirection->File);
    free(Cmd->Redirection);
  }
  free(Cmd);
}

void printCommand(Command *Cmd) {
  if (Cmd == NULL)
    return;
  for (int i = 0; i < Cmd->TokenCount; i++)
    if (i == Cmd->TokenCount - 1)
      printf("%s", Cmd->Tokens[i]);
    else
      printf("%s ", Cmd->Tokens[i]);
  printf("\n");
}

LocalVariableArray *initLocalVariables(int Capacity) {
  LocalVariableArray *VA =
      (LocalVariableArray *)malloc(sizeof(LocalVariableArray));
  if (VA == NULL)
    return NULL;
  VA->Vars = (LocalVariable **)calloc(Capacity, sizeof(LocalVariable *));
  if (VA->Vars == NULL) {
    free(VA);
    fprintf(stderr, "wsh: error initalizing local variables\n");
    exit(1);
  }
  VA->Capacity = Capacity;
  VA->Count = 0;
  return VA;
}

void addLocalVariable(LocalVariableArray *VA, LocalVariable *Var) {
  if (VA == NULL || Var == NULL)
    return;
  if (VA->Count >= VA->Capacity) {
    int NewCapacity = VA->Capacity * 2;
    LocalVariable **NewVars = (LocalVariable **)realloc(
        VA->Vars, NewCapacity * sizeof(LocalVariable *));
    if (NewVars == NULL)
      return;
    VA->Vars = NewVars;
    VA->Capacity = NewCapacity;
  }
  VA->Vars[VA->Count] = Var;
  VA->Count++;
}

int replaceVariables(Command *Cmd, LocalVariableArray *VA, int CheckFirst) {
  if (Cmd == NULL || VA == NULL)
    return 0;
  for (int i = 0; i < Cmd->TokenCount; i++) {
    if (Cmd->Tokens[i][0] != '$')
      continue;
    if (i == 0 && CheckFirst == 1) {
      fprintf(stderr, "local: variable cannot start with $\n");
      return 1;
    }
    if (i == 0 && CheckFirst == 2) {
      fprintf(stderr, "export: variable cannot start with $\n");
      return 1;
    }
    char *Name = Cmd->Tokens[i] + 1;
    char *Value = getenv(Name);
    if (Value != NULL) {
      char *NewValue = strdup(Value);
      if (NewValue == NULL) {
        perror("strdup");
        return 0;
      }
      free(Cmd->Tokens[i]);
      Cmd->Tokens[i] = NewValue;
    } else {
      LocalVariable *Var = getLocalVariable(Name, VA);
      free(Cmd->Tokens[i]);
      if (Var == NULL) {
        Cmd->Tokens[i] = strdup("");
        if (Cmd->Tokens[i] == NULL) {
          perror("strdup");
          return 0;
        }
      } else {
        char *NewValue = strdup(Var->Value);
        if (NewValue == NULL) {
          perror("strdup");
          return 0;
        }
        Cmd->Tokens[i] = NewValue;
      }
    }
  }
  return 0;
}

void updateLocalVariable(LocalVariableArray *VA, LocalVariable *Var) {
  for (int i = 0; i < VA->Count; i++) {
    if (strcmp(VA->Vars[i]->Name, Var->Name) == 0) {
      free(VA->Vars[i]->Value);
      VA->Vars[i]->Value = strdup(Var->Value);
      if (VA->Vars[i]->Value == NULL) {
        perror("strdup");
      }
      return;
    }
  }
}

LocalVariable *getLocalVariable(const char *Name, LocalVariableArray *VA) {
  if (Name == NULL || VA == NULL)
    return NULL;
  for (int i = 0; i < VA->Count; i++)
    if (strcmp(Name, VA->Vars[i]->Name) == 0)
      return VA->Vars[i];
  return NULL;
}

void freeLocalVariableArray(LocalVariableArray *VA) {
  if (VA == NULL)
    return;
  if (VA->Vars) {
    for (int i = 0; i < VA->Count; i++)
      freeLocalVariable(VA->Vars[i]);
    free(VA->Vars);
  }
  free(VA);
}

void freeLocalVariable(LocalVariable *Var) {
  if (Var) {
    free(Var->Name);
    free(Var->Value);
    free(Var);
  }
}

History *initHistory(int NumEntries) {
  History *Hist = (History *)malloc(sizeof(History));
  if (Hist == NULL)
    return NULL;
  Hist->Entries = (Command **)calloc(NumEntries, sizeof(Command *));
  if (Hist->Entries == NULL) {
    free(Hist);
    return NULL;
  }
  Hist->Count = 0;
  Hist->Capacity = NumEntries;
  return Hist;
}

int addHistory(History *Hist, Command *Cmd) {
  Command *PrevCmd = getHistory(1, Hist);
  if (PrevCmd && compareHistory(Cmd, PrevCmd) == 0) {
    freeCommand(PrevCmd);
    return 1;
  }
  freeCommand(PrevCmd);
  if (Hist->Count == Hist->Capacity) {
    freeCommand(Hist->Entries[0]);
    for (int i = 0; i < Hist->Capacity - 1; i++) {
      Hist->Entries[i] = Hist->Entries[i + 1];
    }
    Hist->Count--;
  }
  Hist->Entries[Hist->Count] = Cmd;
  Hist->Count++;
  return 0;
}

int compareHistory(Command *CmdA, Command *CmdB) {
  if (CmdA->TokenCount != CmdB->TokenCount)
    return 1;
  for (int i = 0; i < CmdA->TokenCount; i++)
    if (strcmp(CmdA->Tokens[i], CmdB->Tokens[i]))
      return 1;
  return 0;
}

int setHistoryCapacity(int Capacity, History *Hist) {
  if (Capacity > Hist->Capacity) {
    Command **NewEntries = realloc(Hist->Entries, Capacity * sizeof(Command *));
    if (NewEntries == NULL)
      return 1;
    Hist->Entries = NewEntries;
    Hist->Capacity = Capacity;
  } else {
    Command **NewEntries = (Command **)calloc(Capacity, sizeof(Command *));
    if (NewEntries == NULL)
      return 1;
    int NumRemoveEntries = Hist->Capacity - Capacity;
    int Count = Hist->Count < Capacity ? Hist->Count : Capacity;
    for (int i = 0; i < Count; i++)
      NewEntries[i] = Hist->Entries[NumRemoveEntries + i];
    free(Hist->Entries);
    Hist->Entries = NewEntries;
    Hist->Capacity = Capacity;
    Hist->Count = Count;
  }
  return 0;
}

Command *getHistory(int NumEntry, History *Hist) {
  if (NumEntry < 1 || NumEntry > Hist->Count)
    return NULL;
  return getCommandCopy(Hist->Entries[Hist->Count - NumEntry]);
}

void freeHistory(History *Hist) {
  if (Hist && Hist->Entries) {
    for (int i = 0; i < Hist->Count; i++)
      freeCommand(Hist->Entries[i]);
    free(Hist->Entries);
  }
  free(Hist);
}

int compareStrs(const void *A, const void *B) {
  const char *StrA = *(const char **)A;
  const char *StrB = *(const char **)B;

  while (*StrA && *StrB) {
    if (isalnum(*StrA) && isalnum(*StrB)) {
      if (*StrA != *StrB) {
        return *StrA - *StrB;
      }
      StrA++;
      StrB++;
    } else {
      if (!isalnum(*StrA))
        StrA++;
      if (!isalnum(*StrB))
        StrB++;
    }
  }

  if (*StrA)
    return 1;
  if (*StrB)
    return -1;
  return 0;
}

int filterDirDotFiles(const struct dirent *Entry) {
  return Entry->d_name[0] != '.';
}

int execute(Command *Cmd, Shell *S) {
  if (Cmd == NULL || Cmd->TokenCount == 0 || S == NULL)
    return 1;
  BuiltinCommandInfo *BC = getBuiltinCommandInfo(Cmd);
  if (BC == NULL) {
    Command *CmdCpy = getCommandCopy(Cmd);
    addHistory(S->Hist, CmdCpy);
    if (Cmd->Redirection && Cmd->Redirection->File &&
        Cmd->Redirection->Mode != RedirectNone) {
      free(Cmd->Tokens[Cmd->TokenCount - 1]);
      Cmd->Tokens[--Cmd->TokenCount] = NULL;
    }
    replaceVariables(Cmd, S->VA, 0);
    pid_t PID = fork();
    if (PID == -1) {
      perror("fork");
      return 1;
    }
    if (PID == 0) {
      char *ExecutablePath = findExecutable(Cmd->Tokens[0]);
      if (ExecutablePath == NULL) {
        fprintf(stderr, "command not found: %s\n", Cmd->Tokens[0]);
        freeShell(S);
        freeCommand(Cmd);
        exit(1);
      }
      if (!redirect(Cmd->Redirection)) {
        freeShell(S);
        freeCommand(Cmd);
        exit(1);
      }
      execv(ExecutablePath, Cmd->Tokens);
      perror("execv");
      exit(1);
    } else {
      int Status;
      waitpid(PID, &Status, 0);

      if (WIFEXITED(Status))
        return WEXITSTATUS(Status);
      else
        return 1;
    }
  } else {
    int CheckFirstVar = strcmp(BC->Name, "local") == 0    ? 1
                        : strcmp(BC->Name, "export") == 0 ? 2
                                                          : 0;
    int Err = replaceVariables(Cmd, S->VA, CheckFirstVar);
    if (CheckFirstVar && Err)
      return 1;
    if (Cmd->Redirection && Cmd->Redirection->File &&
        Cmd->Redirection->Mode != RedirectNone) {
      free(Cmd->Tokens[Cmd->TokenCount - 1]);
      Cmd->Tokens[--Cmd->TokenCount] = NULL;
    }
    if (Cmd->Redirection)
      if (!redirect(Cmd->Redirection)) {
        freeCommand(Cmd);
        return 1;
      }
    return BC->Func(Cmd, S);
  }
  return 0;
}

int executeExitCommand(Command *Cmd, Shell *S) {
  int Error = S->Error;
  freeCommand(Cmd);
  freeShell(S);
  exit(-Error);
}

int executeCdCommand(Command *Cmd, Shell *S) {
  if (Cmd == NULL || S == NULL)
    return 1;
  if (Cmd->TokenCount != 2) {
    fprintf(stderr, "cd: usage: 'cd <dir>'\n");
    return 1;
  }
  if (chdir(Cmd->Tokens[1]) != 0) {
    fprintf(stderr, "cd: cannot change to directory '%s'\n", Cmd->Tokens[1]);
    return 1;
  }
  return 0;
}

int executeExportCommand(Command *Cmd, Shell *S) {
  if (Cmd == NULL || S == NULL)
    return 1;
  if (Cmd->TokenCount != 2) {
    fprintf(stderr, "export: usage: 'export S->VAR=<value>'\n");
    return 1;
  }
  char *name = strtok(Cmd->Tokens[1], "=");
  if (name == NULL)
    return 1;
  char *value = strtok(NULL, "=");
  if (value == NULL) {
    fprintf(stderr, "export: variable must have definition\n");
    return 1;
  }
  if (setenv(name, value, 1) < 0)
    return 1;
  return 0;
}

int executeLocalCommand(Command *Cmd, Shell *S) {
  if (Cmd == NULL || S == NULL)
    return 1;
  if (Cmd->TokenCount != 2) {
    fprintf(stderr, "local: usage: 'local S->VAR=<value>'\n");
    return 1;
  }
  char *TokenCopy = strdup(Cmd->Tokens[1]);
  if (!TokenCopy) {
    perror("strdup");
    return 1;
  }
  char *Name = strtok(TokenCopy, "=");
  if (Name == NULL) {
    free(TokenCopy);
    fprintf(stderr, "local: usage: 'local S->VAR=<value>'\n");
    return 1;
  }
  char *Value = strtok(NULL, "=");
  LocalVariable *Var = (LocalVariable *)malloc(sizeof(LocalVariable));
  if (!Var) {
    free(TokenCopy);
    return 1;
  }
  Var->Name = strdup(Name);
  if (Var->Name == NULL) {
    free(Var);
    free(TokenCopy);
    perror("strdup");
    return 1;
  }
  Var->Value = Value == NULL ? strdup("") : strdup(Value);
  if (Var->Value == NULL) {
    free(Var->Name);
    free(Var);
    free(TokenCopy);
    perror("strdup");
    return 1;
  }
  free(TokenCopy);
  LocalVariable *ExistingVar = getLocalVariable(Var->Name, S->VA);
  if (ExistingVar == NULL)
    addLocalVariable(S->VA, Var);
  else
    updateLocalVariable(S->VA, Var);
  return 0;
}

int executeVarsCommand(Command *Cmd, Shell *S) {
  if (Cmd == NULL || S == NULL)
    return 1;
  if (Cmd->TokenCount != 1) {
    fprintf(stderr, "vars: usage: 'vars'\n");
    return 1;
  }
  for (int i = 0; i < S->VA->Count; i++)
    if (S->VA->Vars[i]->Name != NULL && S->VA->Vars[i]->Value != NULL)
      printf("%s=%s\n", S->VA->Vars[i]->Name, S->VA->Vars[i]->Value);
  return 0;
}

int executeHistoryCommand(Command *Cmd, Shell *S) {
  if (Cmd == NULL || S == NULL)
    return 1;
  History *Hist = S->Hist;
  if (Hist == NULL)
    return 1;
  switch (Cmd->TokenCount) {
  default:
    fprintf(stderr, "history: incorrect usage\n");
    return 1;
  case 1: {
    for (int i = 1; i <= Hist->Count; i++) {
      printf("%d) ", i);
      Command *Temp = getHistory(i, Hist);
      printCommand(Temp);
      freeCommand(Temp);
    }
  } break;
  case 2: {
    char *EndPtr;
    int NumEntry = (int)strtol(Cmd->Tokens[1], &EndPtr, 0);
    if (Cmd->Tokens[1] == EndPtr) {
      fprintf(stderr, "history: usage: 'history <n>'\n");
      return 1;
    }
    Command *NextCmd = getHistory(NumEntry, Hist);
    return NextCmd == NULL ? 1 : execute(NextCmd, S);
  } break;
  case 3: {
    if (strcmp(Cmd->Tokens[1], "set")) {
      fprintf(stderr, "history: usage: 'history set <n>'\n");
      return 1;
    }
    char *EndPtr;
    int Capacity = (int)strtol(Cmd->Tokens[2], &EndPtr, 0);
    if (Cmd->Tokens[2] == EndPtr) {
      fprintf(stderr, "history: usage: 'history set <n>'\n");
      return 1;
    }
    if (Capacity < 1) {
      fprintf(stderr, "history: minimum history is 1\n");
      return 1;
    }
    return setHistoryCapacity(Capacity, Hist);
  } break;
  }
  return 0;
}

int executeLsCommand(Command *Cmd, Shell *S) {
  if (Cmd == NULL || S == NULL)
    return 1;
  if (Cmd->TokenCount != 1) {
    fprintf(stderr, "ls: usage: 'ls'\n");
    return 1;
  }
  struct dirent **NameList;
  int NumEntries = scandir(".", &NameList, filterDirDotFiles, alphasort);
  if (NumEntries < 0) {
    perror("scandir");
    return 1;
  }

  for (int i = 0; i < NumEntries; i++) {
    printf("%s\n", NameList[i]->d_name);
    free(NameList[i]);
  }
  free(NameList);
  return 0;
}
