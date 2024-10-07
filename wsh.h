#ifndef WSH_H
#define WSH_H

#include <dirent.h>
#include <sys/types.h>

typedef enum {
  RedirectNone,
  RedirectInput,
  RedirectOutput,
  RedirectAppend,
  RedirectOuputError,
  RedirectAppendError,
} RedirectMode;

typedef struct {
  RedirectMode Mode;
  char *File;
  int FD;
} Redirect;

typedef struct {
  int Flags;
  mode_t Mode;
} RedirectFlag;

typedef struct {
  char **Tokens;
  int TokenCount;
  Redirect *Redirection;
} Command;

typedef struct {
  Command **Entries;
  int Count;
  int Capacity;
} History;

typedef struct {
  char *Name;
  char *Value;
} LocalVariable;

typedef struct {
  LocalVariable **Vars;
  int Count;
  int Capacity;
} LocalVariableArray;

typedef struct {
  LocalVariableArray *VA;
  History *Hist;
  int Error;
} Shell;

typedef int (*BuiltinCommandFunc)(Command *, Shell *);

typedef struct {
  const char *Name;
  BuiltinCommandFunc Func;
} BuiltinCommandInfo;

extern const BuiltinCommandInfo BuiltinCommandInfoMap[];
extern const int NumBuiltinCommands;
extern const RedirectFlag RedirectFlags[];

Shell *initShell(void);
void freeShell(Shell *);

int runInteractiveMode(Shell *);
int runBatchMode(Shell *, const char *);

char *findExecutable(const char *);
int openRedirect(Redirect *);
int redirect(Redirect *);
void freeRedirect(Redirect *);

LocalVariableArray *initLocalVariables(int);
void addLocalVariable(LocalVariableArray *, LocalVariable *);
int replaceVariables(Command *, LocalVariableArray *, int);
void updateLocalVariable(LocalVariableArray *, LocalVariable *);
LocalVariable *getLocalVariable(const char *, LocalVariableArray *);
void freeLocalVariableArray(LocalVariableArray *);
void freeLocalVariable(LocalVariable *);

Command *getCommand(char *, LocalVariableArray *);
Command *getCommandCopy(Command *);
BuiltinCommandInfo *getBuiltinCommandInfo(Command *);
void freeCommand(Command *);
void printCommand(Command *);

History *initHistory(int);
int addHistory(History *, Command *);
int compareHistory(Command *CommandA, Command *CommandB);
int setHistoryCapacity(int, History *);
Command *getHistory(int, History *);
void freeHistory(History *);

int compareStrs(const void *a, const void *b);
int filterDirDotFiles(const struct dirent *);

int execute(Command *, Shell *);
int executeExitCommand(Command *, Shell *);
int executeCdCommand(Command *, Shell *);
int executeExportCommand(Command *, Shell *);
int executeLocalCommand(Command *, Shell *);
int executeVarsCommand(Command *, Shell *);
int executeHistoryCommand(Command *, Shell *);
int executeLsCommand(Command *, Shell *);

#endif
