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
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t startPosition = ftell(f);
    int32_t size = (int32_t)text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.c_str(), 1, size, f);
    return startPosition;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    // returns -1 if there is no record left (end of file)
    int64_t offsetField;
    int32_t size;
    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
    {
        return -1;
    }
    if (fread(&size, sizeof(int32_t), 1, f) != 1)
    {
        return -1;
    }
    char* buffer = new char[size + 1];
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    outText = buffer;
    delete[] buffer;
    return offsetField;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error
    // A call line stores -1 first, and then position of the function.

    ifstream in(sourcePath);
    if (!in)
    {
        setError("cannot open source file");
        return -1;
    }
    FILE* out;
    fopen_s(&out, resolveBinPath, "w+b");
    if (out == NULL)
    {
        setError("cannot create resolve.bin");
        return -1;
    }

    string line;
    while (readSourceLine(in, line))
    {
        string word = firstWord(line);
        int64_t position = ftell(out);

        if (word == "func")
        {
            string name = secondWord(line);
            for (int32_t i = 0; i < funcCount; i++)
            {
                if (funcArray[i].funcName == name)  //duplicate function check
                {
                    setError("function " + name + " is defined twice");
                    fclose(out);
                    return -1;
                }
            }
            if (funcCount >= MAX_FUNCS)
            {
                setError("too many functions");
                fclose(out);
                return -1;
            }

            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = position;
            funcCount++;
            writeResolveRecord(out, position, line);
        }
        else if (word == "call")
        {
            string target = secondWord(line);
            if (target == "")
            {
                setError("call has no function name");
                fclose(out);
                return -1;
            }
            if (patchCount >= MAX_PATCHES)
            {
                setError("too many call lines");
                fclose(out);
                return -1;
            }

            patches[patchCount].byteOffsetOfOffsetField = position;
            patches[patchCount].targetFuncName = target;
            patchCount++;
            writeResolveRecord(out, -1, line); 
        }
        else
        {
            writeResolveRecord(out, position, line);
        }
    }

    for (int32_t p = 0; p < patchCount; p++){
        int64_t targetOffset = -1;
        for (int32_t i = 0; i < funcCount; i++){
            if (funcArray[i].funcName == patches[p].targetFuncName){
                targetOffset = funcArray[i].byteOffsetInResolveBin;
            }
        }
    
        if (targetOffset == -1){
            setError("call to undefined function " + patches[p].targetFuncName);
            fclose(out);
            return -1;
        }

        fseek(out, (int64_t)patches[p].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOffset, sizeof(int64_t), 1, out);
    
    }
    fclose(out);

    for (int32_t i = 0; i < funcCount; i++){
        if (funcArray[i].funcName == "main"){
            return funcArray[i].byteOffsetInResolveBin;
        }
    }
    setError("No main function, nalle");
    return -1;
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
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    // returns how many tokens were found, or -1 if the line has too many words
    int32_t count = 0;
    int32_t i = 0;
    int32_t n = (int32_t)line.size();

    while (i < n)
    {
        // skip spaces
        while (i < n && isSpace(line[i]))
        {
            i++;
        }
        if (i >= n)
        {
            break;
        }
        // read one word
        int32_t start = i;
        while (i < n && !isSpace(line[i]))
        {
            i++;
        }

        if (count >= maxTokens)
        {
            return -1;
        }
        if (count == 0)
        {
            tokens[count].type = KEYWORD;
        }
        else if (count == 1)
        {
            tokens[count].type = IDENTIFIER;
        }
        else
        {
            tokens[count].type = PARAM;
        }
        tokens[count].text = line.substr(start, i - start);
        count++;
    }
    return count;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
    // (callStack[0] in the snapshot is the top of the live stack)
    Snapshot* s = new Snapshot;
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}


bool isNumber(const string& word)       //integers greater than 9
{
    int32_t start = 0;
    if (word.size() > 0 && word[0] == '-')
    {
        start = 1;
    }
    if (start >= (int32_t)word.size())
    {
        return false;
    }
    for (int32_t i = start; i < (int32_t)word.size(); i++)
    {
        if (word[i] < '0' || word[i] > '9')
        {
            return false;
        }
    }
    return true;
}

Variable* findVariable(Frame& frame, const string& name)
{
    for (int32_t i = 0; i < frame.argc; i++)
    {
        if (frame.argv[i].name == name)
        {
            return &frame.argv[i];
        }
    }
    for (int32_t i = 0; i < frame.localCount; i++)
    {
        if (frame.locals[i].name == name)
        {
            return &frame.locals[i];
        }
    }
    return NULL;
}

bool getValue(Frame& frame, const string& word, int32_t& result)
{
    if (isNumber(word))
    {
        result = atoi(word.c_str());
        return true;
    }
    Variable* v = findVariable(frame, word);
    if (v == NULL)
    {
        return false;
    }
    result = v->value;
    return true;
}

bool doSetOrMath(Frame& current, Token tokens[], int32_t n, const string& line)
{
    string keyword = tokens[0].text;
    string varName = tokens[1].text;

    if (n < 3)
    {
        setError("missing value in: " + line);
        return false;
    }
    int32_t number;
    if (!getValue(current, tokens[2].text, number))
    {
        setError("unknown variable " + tokens[2].text + " in: " + line);
        return false;
    }
    Variable* v = findVariable(current, varName);

    if (keyword == "set")
    {
        if (v == NULL)
        {
            if (current.localCount >= MAX_VARS_PER_FRAME)
            {
                setError("too many variables in function " + current.func_name);
                return false;
            }
            v = &current.locals[current.localCount];
            v->name = varName;
            current.localCount++;
        }
        v->value = number;
        return true;
    }

    if (v == NULL)
    {
        setError("unknown variable " + varName + " in: " + line);
        return false;
    }
    if (keyword == "add")
    {
        v->value = v->value + number;
    }
    else if (keyword == "sub")
    {
        v->value = v->value - number;
    }
    else if (keyword == "mul")
    {
        v->value = v->value * number;
    }
    else
    {
        if (number == 0)
        {
            setError("division by zero in: " + line);
            return false;
        }
        v->value = v->value / number;
    }
    return true;
}

// handles: call   (makes a new frame for the function and pushes it)
// callStart   = where this call line starts in resolve.bin
// targetOffset = where the called function starts (patched in pass 0x1)
bool doCall(FILE* f, Stack<Frame>& callStack, int64_t callPos[], Token tokens[], int32_t n,int64_t callStart, int64_t targetOffset)
{
    if (targetOffset == -1)                         //new frames where a frame starts where a call dunction starts
    {
        setError("call was not resolved");
        return false;
    }
    if (callStack.depth() >= MAX_STACK_DEPTH)
    {
        setError("stack overflow: too many nested calls");
        return false;
    }

    int32_t argCount = n - 2;
    int64_t returnPosition = ftell(f);
    Frame& caller = callStack.peek();

    fseek(f, (long)targetOffset, SEEK_SET);
    string headerLine;
    readResolveRecord(f, headerLine);
    Token header[MAX_TOKENS];
    int32_t headerCount = tokenizeLine(headerLine, header, MAX_TOKENS);
    if (headerCount < 2)
    {
        setError("bad function line: " + headerLine);
        return false;
    }
    if (headerCount - 2 != argCount)
    {
        setError("wrong number of arguments for function " + header[1].text);
        return false;
    }

    Frame callee;
    callee.func_name = header[1].text;
    callee.argc = argCount;
    callee.returnLine = (int32_t)returnPosition;
    callee.localCount = 0;
    for (int32_t i = 0; i < argCount; i++)
    {
        int32_t value;
        if (!getValue(caller, tokens[i + 2].text, value))
        {
            setError("unknown variable " + tokens[i + 2].text + " in call");
            return false;
        }
        callee.argv[i].name = header[i + 2].text;
        callee.argv[i].value = value;
    }

    callPos[callStack.depth()] = callStart;
    callStack.push(callee);
    return true;
}

void doReturn(FILE* f, Stack<Frame>& callStack, int64_t callPos[])          //return 
{
    int32_t depth = callStack.depth();
    Frame callee = callStack.pop();
    Frame& caller = callStack.peek();

    fseek(f, (long)callPos[depth - 1], SEEK_SET);
    string callLine;
    readResolveRecord(f, callLine);
    Token callTokens[MAX_TOKENS];
    tokenizeLine(callLine, callTokens, MAX_TOKENS);

    for (int32_t i = 0; i < callee.argc; i++)
    {
        string passed = callTokens[i + 2].text;
        if (isNumber(passed))
        {
            continue;
        }
        Variable* v = findVariable(caller, passed);
        if (v != NULL)
        {
            v->value = callee.argv[i].value;
        }
    }

    fseek(f, (long)callee.returnLine, SEEK_SET);
}

void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
    //
    // A snapshot is taken after every executed line (set, add, sub, mul, div, call, func_end).
    // When main reaches its func_end the program is finished.
    FILE* f;
    fopen_s(&f, resolveBinPath, "rb");
    if (f == NULL)
    {
        setError("cannot open resolve.bin");
        return;
    }

    Stack<Frame> callStack;
    int64_t callPos[MAX_STACK_DEPTH]; 
    Token tokens[MAX_TOKENS];
    string line;
    fseek(f, (long)mainOffset, SEEK_SET);
    readResolveRecord(f, line);
    int32_t n = tokenizeLine(line, tokens, MAX_TOKENS);
    if (n < 2)
    {
        setError("bad main line");
        fclose(f);
        return;
    }
    Frame mainFrame;
    mainFrame.func_name = tokens[1].text;
    mainFrame.argc = n - 2;
    for (int32_t i = 0; i < mainFrame.argc; i++)
    {
        mainFrame.argv[i].name = tokens[i + 2].text;
        mainFrame.argv[i].value = 0;
    }
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;
    callStack.push(mainFrame);
    callPos[0] = -1;

    int32_t steps = 0;
    while (true)
    {
        int64_t lineStart = ftell(f);
        int64_t offsetField = readResolveRecord(f, line);
        if (offsetField == -1 && feof(f))
        {
            setError("program ended without func_end of main");
            break;
        }

        n = tokenizeLine(line, tokens, MAX_TOKENS);
        if (n < 1)
        {
            setError("bad line: " + line);
            break;
        }

        string keyword = tokens[0].text;
        bool ok = true;
        bool finished = false;

        if (keyword == "set" || keyword == "add" || keyword == "sub" || keyword == "mul" || keyword == "div")
        {
            ok = doSetOrMath(callStack.peek(), tokens, n, line);
        }
        else if (keyword == "call")
        {
            ok = doCall(f, callStack, callPos, tokens, n, lineStart, offsetField);
        }
        else if (keyword == "func_end")
        {
            if (callStack.depth() == 1)
            {
                finished = true; // main func_end
            }
            else
            {
                doReturn(f, callStack, callPos);
            }
        }
        else
        {
            setError("cannot run this line: " + line);
            ok = false;
        }

        if (!ok)
        {
            break;
        }

        timeline.record(buildSnapshot(callStack));  //snaps ni rukni chahiyai
        steps++;

        if (finished)
        {
            break;
        }
        if (steps >= MAX_STEPS)
        {
            setError("program runs for too many steps");
            break;
        }
    }

    fclose(f);
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