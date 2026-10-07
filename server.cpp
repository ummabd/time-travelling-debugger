// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = NULL;
        count = 0;
    }
    ~Stack()
    {
        // free every node that is still in the stack
        while (top != NULL)
        {
            Node* temp = top;
            top = top->next;
            delete temp;
        }
    }
    void push(const T& val)
    {
        // pushes the value on the stack if max limit is not reached yet.
        if (count >= MAX_STACK_DEPTH)
        {
            return;
        }
        Node* newNode = new Node;
        newNode->data = val;
        newNode->next = top;
        top = newNode;
        count++;
    }
    T pop()
    {
        // pop the top value on the stack
        if (top == NULL)
        {
            return T();
        }
        Node* temp = top;
        T value = temp->data;
        top = top->next;
        delete temp;
        count--;
        return value;
    }
    T& peek()
    {
        // returns the top value on the stack (check isEmpty() before calling)
        return top->data;
    }
    bool isEmpty()
    {
        return top == NULL;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        int32_t done = 0;
        Node* current = top;
        while (current != NULL && done < maxLen)
        {
            out[done] = current->data;
            done++;
            current = current->next;
        }
        return done;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head;
    TimelineNode* tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = NULL;
        tail = NULL;
        stepCount = 0;
    }
    ~Timeline()
    {
        TimelineNode* temp = head;
        while (temp != NULL)
        {
            TimelineNode* nyanode = temp->next;
            delete temp->data;
            delete temp;
            temp = nyanode;
        }
    }
    void record(Snapshot* s)
    {
        // add record in the timeline (always at the end)
        TimelineNode* nyanode = new TimelineNode;
        nyanode->data = s;
        nyanode->next = NULL;
        nyanode->prev = tail;
        if (tail == NULL)
        {
            head = nyanode; // first node
        }
        else
        {
            tail->next = nyanode;
        }
        tail = nyanode;
        stepCount++;
    }
    TimelineNode* begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, sizeof(char), 4, f);
    fwrite(&h.version, sizeof(int32_t) , 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool noneed(char c)                                //helper
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool readSourceLine(ifstream& in, string& out)
{
    // reads the next nonblank line
    string line;
	while (getline(in, line))                   //checks for empty lines and comments, and ignores leading / trailing space
    {
        int32_t start = 0;
        int32_t end = (int32_t)line.size();
        while (start < end && noneed(line[start]))
        {
            start++;
        }
        while (end > start && noneed(line[end - 1]))
        {
            end--;
        }

        if (start == end)
        {
            continue; 
        }
        if (end - start >= 2 && line[start] == '/' && line[start + 1] == '/')
        {
            continue; 
        }

        out = line.substr(start, end - start);
        return true;
    }
    return false; // no more lines
}
string firstWord(const string& line)
{  // returns first word from the input string
    
    int32_t i = 0;
    int32_t n = (int32_t)line.size();
    while (i < n && noneed(line[i]))   i++;
  
    int32_t start = i;
    while (i < n && !noneed(line[i]))  i++;
    
    return line.substr(start, i - start);
}
string secondWord(const string& line)
{
    // returns the second word
    int32_t i = 0;
    int32_t n = (int32_t)line.size();
    
    while (i < n && noneed(line[i]))    i++;
    
    while (i < n && !noneed(line[i]))
    {
        i++;
    }
    // skip spaces, then read the second word
    while (i < n && noneed(line[i]))   i++;
    int32_t start = i;
    while (i < n && !noneed(line[i]))  i++;


    return line.substr(start, i - start);
}
bool isKeyword(const string& word)
{
    return word == "func" || word == "func_end" || word == "call" || word == "set" || word == "add" || word == "sub" || word == "mul" || word == "div";
}




bool validateProgram(const char* sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed -
    ifstream in(sourcePath);
    if (!in)
    {
        setError("cannot open source file");
        return false;
    }

    bool insideFunc = false; 
    string currentFunc;
    string line;

    while (readSourceLine(in, line))
    {
        string word = firstWord(line);

        if (!isKeyword(word))
        {
            setError("unknown instruction: " + line);
            return false;
        }

        if (word == "func")
        {
            string name = secondWord(line);
            if (name == "")
            {
                setError("func has no name");
                return false;
            }
            if (insideFunc)
            {
                setError("function " + name + " is written inside function " + currentFunc);
                return false;
            }
            insideFunc = true;
            currentFunc = name;
        }
        else if (word == "func_end")
        {
            if (!insideFunc)
            {
                setError("func_end without a func");
                return false;
            }
            insideFunc = false;
        }
    }

    if (insideFunc)
    {
        setError("function " + currentFunc + " has no func_end");
        return false;
    }
    return true;
}




// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is done, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}