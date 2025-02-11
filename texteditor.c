#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <stdarg.h>

/*MACROS*/

/*
converte o charatecer para um de controle fazendo um AND bitwise

0x1F (binary 00011111) is a bitmask that preserves only the  
lower 5 bits of a character.
*/
#define CTRL_KEY(k) ((k) & 0x1f)
#define TE_VERSION ":)"
#define TAB_STOP 8
#define SAVE_MACRO "CTRL-s"
#define QUIT_MACRO "CTRL-q"
#define QUIT_TIMES 2

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
  };


/* DATA */

/*
editor row, stores a line of text as a pointer to the dynam alloc char data
and a lenght
*/
typedef struct erow{
    int size;
    int rsize;
    char *chars;
    char *render;
}erow;

typedef struct editorConfig{
    int cx, cy;
    int rx; //cx on the render field
    int rowoffset, coloffset;
    int screenrows, screencols;
    int numrows;
    erow *row;
    int dirty;
    struct termios orig_termios;
    char statusmsg[80];
    time_t statusmsg_time;
    char *filename;
}editorConfig;

editorConfig E;

/* PROTOTYPES */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshSCreen();
char *editorPrompt(char *prompt);

/* TERMINAL */

void die(const char *s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(EXIT_FAILURE);
}

void disableRawMode(){

    //Checka se tem erro ao setar os atributos da origem
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1){
        die("tcsetattr");
    }
}

void enableRawMode(){
    /*
    Raw mode eh ler caractere por caractere
    Diferente do canonical mode (modo padrao de terminal)
    que processa apos o enter
    */

    //Checka se tem erro ao pegar os atributos da origiem
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1){
        die("tcgetattr");
    }

    atexit(disableRawMode);

    //Criando uma copia do estado atual do terminal para
    //quando encerrar o programa ele esteja do mesmo estado
    struct termios raw = E.orig_termios;

    //Desabilitando algumas flags para deixar em Raw mode
    raw.c_iflag &= ~(ICRNL|IXON|ISTRIP|INPCK|BRKINT);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN|ISIG);

    //Vmin: numero minimo de bytes para que read()
    //possa retornar
    raw.c_cc[VMIN] = 0;
    
    //Vtime: tempo maximo para que o read retorne
    //1 = 1/10 segundos
    raw.c_cc[VTIME] = 1;

    //Checka se tem erro ao setar os atributos da raw
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1){
        die("tcsetattr");
    } 
}

int editorReadKey(){
    int nread;
    char c;

    while((nread = read(STDIN_FILENO, &c, 1))!=  1){
        if(nread == -1 && errno != EAGAIN){
            die("read");
        }
    }

    //Logica para lidar com os inputs
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
          if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if (seq[2] == '~') {
              switch (seq[1]) {
                //Page Up is sent as <esc>[5~ and Page Down is sent as <esc>[6~, etc...
                case '1': return HOME_KEY;
                case '3': return DEL_KEY;
                case '4': return END_KEY;
                case '5': return PAGE_UP;
                case '6': return PAGE_DOWN;
                case '7': return HOME_KEY;
                case '8': return END_KEY;
              }
            }
          } else {
            switch (seq[1]) {
              case 'A': return ARROW_UP;
              case 'B': return ARROW_DOWN;
              case 'C': return ARROW_RIGHT;
              case 'D': return ARROW_LEFT;
              case 'H': return HOME_KEY;
              case 'F': return END_KEY;
            }
          }
        } else if (seq[0] == 'O') {
          switch (seq[1]) {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
          }
        }
        return '\x1b';
      } else {
        return c;
      }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
      if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
      if (buf[i] == 'R') break;
      i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);;
    }else{
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return EXIT_SUCCESS;
    }
}

/* ROW OPERATIONS */

int editorRowCxtoRx(erow *row, int cx){
    int rx = 0;
    /*
    For each character, if it’s a tab we use rx % TAB_STOP to find out how many columns 
    we are to the right of the last tab stop, and then subtract that from TAB_STOP - 1 to 
    find out how many columns we are to the left of the next tab stop. 
    */

    for(int i=0;i<cx;i++){
        if(row->chars[i] == '\t'){
            rx+=(TAB_STOP - 1) - (rx % TAB_STOP);
        }
        rx++;
    }
    return rx;
}


void editorUpdateRow(erow *row){
    int tabs = 0;
    for(int i=0;i<row->size;i++){
        if(row->chars[i] == '\t') tabs++;
    }
    free(row->render);
    //The maximum number of characters needed for each tab is 8
    row->render = malloc(row->size +tabs*(TAB_STOP-1) +1);
    int j = 0;
    for(int i=0;i<row->size;++i){
        if(row->chars[i] == '\t'){
            row->render[j++] = ' ';
            //tab stop is a column that is divisible by 8
            while(j%TAB_STOP!=0) row->render[j++] = ' ';
        }
        else row->render[j++] = row->chars[i];
    }
    row->render[j] = '\0';
    row->rsize = j;
}

void editorInsertRow(int at, char *s, size_t len){
    if(at<0||at>E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows+1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    //int at = E.numrows; //Indice da nova linha que queremos inicializar
    E.row[at].size = len;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row){
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at){
    if(at<0||at>E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.numrows-at-1));
    E.numrows--;
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len){
    row->chars = realloc(row->chars, row->size + len +1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}   

void editorRowInsertChar(erow *row, int at, int c){
    if(at<0||at>row->size){
        at = row->size;
    }
    row->chars = realloc(row->chars, row->size+2); //2 pq tem o nullbyte
    memmove(&row->chars[at+1], &row->chars[at], row->size - at +1); //Tipo um memcpy mas mais seguro
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at){
    if(at < 0 || at >= row->size) return;

    /*
        memmove(destination, source, size)
    */

    memmove(&row->chars[at],&row->chars[at+1], row->size-at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}
/* EDITOR OPERATIONS */
void editorInsertChar(int c){
    if(E.cy == E.numrows){
        editorInsertRow(E.numrows, "", 0); //se chegar no final das linhas add uma nova
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

//Funtion to handle the "enter" key
void editorInsertNewline(){
    if(E.cx == 0){
        editorInsertRow(E.cy, "", 0);
    }else{
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy+1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar(){
    if(E.cy == E.numrows) return;
    if(E.cx == 0 && E.cy == 0) return;
    erow *row = &E.row[E.cy];
    if(E.cx>0){
        editorRowDelChar(row, E.cx-1);
        E.cx--;
    }else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
      }
}
/* File I/O*/

//Transformando as linhas do arquivo em uma string so para salvar
char *editorRowToString(int *buflen){
    int totlen = 0;
    //Adicioanndo o tamanho de cada linha
    for(int i=0;i<E.numrows;i++){
        totlen += E.row[i].size+1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    
    for(int i=0;i<E.numrows;i++){
        /*
            Loop pelas linhas e copiando o conteudo para o final do buffer
            adicionando um caracter de quebra de linha apos
        */

        memcpy(p, E.row[i].chars, E.row[i].size);
        p+=E.row[i].size;
        *p = '\n';
        p++;
    }
    return buf;
}


void editorOpen(char *filename){
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");
    
    char *line = NULL;
    ssize_t linecap = 0;
    ssize_t linelen;
    //Loop para ler as linhas todas do arquivo
    //getline retorna -1 quando chegar no fim do arquivo
    while((linelen = getline(&line, &linecap, fp)) != -1){ 
        while(linelen>0 && (line[linelen-1]=='\n' || line[linelen-1] =='\r')){
            linelen--;
        }
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave(){
    if(E.filename == NULL){
        E.filename = editorPrompt("Save as: %s");
        if(E.filename == NULL){
            editorSetStatusMessage("Save cancelled by the user");
            return;
        }
    }
    int len;
    char *buf = editorRowToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    //Error handling
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
          if (write(fd, buf, len) == len) {
            close(fd);
            free(buf);
            E.dirty = 0;
            editorSetStatusMessage("%d bytes were written to disk", len);
            return;
          }
        }
        close(fd);
      }
    free(buf);
    editorSetStatusMessage("Can't save, error: %s", strerror(errno));
}

/* append buffer*/

typedef struct abuf{
    char *b;
    int len;
}abuf;

#define ABUF_INIT {NULL, 0}

void abAppend(abuf *ab, const char *s, int len){
    //realoca o buffer para o novo tamanho
    char *new = realloc(ab->b, ab->len + len);
    if(new == NULL) return;

    //&new[ab->len] copiar dps do que ja tem
    memcpy(&new[ab->len], s, len);

    //atualiza as variaveis
    ab->b = new;
    ab->len = ab->len + len;
}

void abFree(abuf *ab){
    free(ab->b);
}

/* INPUT */

//Prompt to save the file
char *editorPrompt(char *prompt){
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshSCreen();

        int c = editorReadKey();
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if(buflen!=0) buf[--buflen] = '\0';
        }else if(c == '\x1b'){
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        }else if(c == '\r'){
            if(buflen != 0){
                editorSetStatusMessage("");
                return buf;
            }
        }else if(!iscntrl(c) && c < 128){
            if(buflen == bufsize -1){
                bufsize*=2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}


void editorProcessKeypress(){
    int c = editorReadKey();
    static int quit_times = QUIT_TIMES;
    //Break se der ctrl q
    switch(c){
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("File has unsaved changes!!. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY: //Returns to the start of the line
            E.cx=0;
            break;

        case END_KEY:
            if(E.cy < E.numrows) E.cx = E.row[E.cy].size; //Goes to the end of the line
            break;

        case PAGE_UP:
        case PAGE_DOWN:
        {
           if (c == PAGE_UP){
            E.cy = E.rowoffset; //Goes to the top of the screen
           }else if(c == PAGE_DOWN){ //Goest to the bottom of the screen
            E.cy = E.rowoffset + E.screenrows - 1;
            if(E.cy > E.numrows) E.cy = E.numrows;
           }
        }
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case 'r':
            editorInsertNewline();
            break;  
             
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }
    quit_times = QUIT_TIMES;
}

void editorMoveCursor(int key){
    //Se o cursor tiver em uma linha, o ponteiro vai apontar para o erow q ele ta
    erow *row = (E.cy > E.numrows) ? NULL : &E.row[E.cy];

    switch(key){
        case ARROW_LEFT:
            if(E.cx!=0) E.cx--;
            else if(E.cy > 0){
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break; 
        case ARROW_RIGHT:
            if(row && E.cx < row->size) E.cx++;
            else if(row && E.cx == row->size){
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows) E.cy++;
            break;
    }

    row = (E.cy > E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if(E.cx > rowlen){
        E.cx = rowlen;
    }
}

/* OUTPUT */

void editorScroll(){
    E.rx = 0;
    if(E.cy<E.numrows){
        E.rx = editorRowCxtoRx(&E.row[E.cy], E.cx);
    }
    if(E.cy < E.rowoffset){
        E.rowoffset = E.cy;
    }
    //E.rowoffset refers to whats at the top of the screen
    //Checkar se o cursor ta embaixo de onde eh visivel
    if(E.cy >= E.rowoffset + E.screenrows){
        E.rowoffset = E.cy - E.screenrows +1;
    }
    if(E.rx < E.coloffset){
        E.coloffset = E.rx;
    }
    if(E.rx >= E.coloffset + E.screencols){
        E.coloffset = E.rx - E.screencols+1;
    }

}

void editorDrawRows(abuf *ab){
    for(int i=0;i<E.screenrows;i++){
        int filerow = i+E.rowoffset;
        if(filerow>=E.numrows){ 
            if(E.numrows == 0 && i == E.screenrows/3){
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                "%s", TE_VERSION);
                if(welcomelen>E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                abAppend(ab, "~", 1);
                padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }  
            else abAppend(ab, "~", 1);
        }else {
            int len = E.row[filerow].rsize - E.coloffset;
            if(len<0) len=0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloffset], len);
        }
        abAppend(ab, "\x1b[K", 3); //dar clear linha por linha ao inves da tela inteira       
        //if(i<E.screenrows-1){
            abAppend(ab, "\r\n", 2);
        //}
    }
}

void editorDrawStatusBar(abuf *ab){
    //\x1b[7m: Modo de inversão de cores no terminal (background preto e texto branco invertido).
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    //parte esquerda da barra de status
    int len =snprintf(status, sizeof(status), "%.20s - %d lines", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : " ");
    //parte direita da barra de status
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d", E.cy+1);                
    if(len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    //preencher o meio da barra com espaços
    while(len < E.screencols){
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        }else{
            abAppend(ab, " ", 1);
            len++;
        }  
    }
    //volta a cor normal do terminal
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);

}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
      abAppend(ab, E.statusmsg, msglen);
  }


void editorRefreshSCreen(){
    editorScroll();
    //"\x1b" is a escape character
    //We are sending a escape sequence to clear the entire screen
    //[2 (clear entire screen), [1 untill the cursor, [0 from the cursor to the end 
    abuf ab = ABUF_INIT;
    
    abAppend(&ab, "\x1b[?25l", 6); //Hide the cursor
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoffset) + 1, (E.rx - E.coloffset)+ 1);
    abAppend(&ab, buf, strlen(buf));

    //abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6); //Show the cursor
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt,...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);

}


/* INIT */

//Init all the fields in the E struct
void initEditor(){
    E.cx = E.cy = E.numrows = 0;
    E.rx = 0;
    E.rowoffset = E.coloffset = 0;
    E.row = NULL;
    E.filename = NULL;
    if(getWindowSize(&E.screenrows, &E.screencols) == -1){
        die("getWindowSize");
    }
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.screenrows-=2;
    E.dirty = 0;
}

int main(int argc, char **argv){

    enableRawMode();
    initEditor();
    if(argc>=2)editorOpen(argv[1]);

    editorSetStatusMessage("Ctrl-Q = quit | Ctrl-S = save");
    char c;
    while (1) {
        editorRefreshSCreen();
        editorProcessKeypress();
    }
    return 0;
}