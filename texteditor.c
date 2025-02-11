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


/*MACROS*/

/*
converte o charatecer para um de controle fazendo um AND bitwise

0x1F (binary 00011111) is a bitmask that preserves only the  
lower 5 bits of a character.
*/
#define CTRL_KEY(k) ((k) & 0x1f)
#define TE_VERSION "0.0.1"

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
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
    char *chars;
}erow;

typedef struct editorConfig{
    int cx, cy;
    int screenrows, screencols;
    int numrows;
    erow row;
    struct termios orig_termios;
}editorConfig;

editorConfig E;

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

    //Lidar com o input das setas
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                  switch (seq[1]) {
                    //Page Up is sent as <esc>[5~ and Page Down is sent as <esc>[6~.
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                    case '3': return DEL_KEY;
                  }
                }
            }else{
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                }
            }
        return '\x1b';
        }else return c;
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

/* File I/O*/

void editorOpen(char *filename){
    FILE *fp = fopen(filename, 'r');
    if(!fp) die("fopen");
    
    char *line = NULL;
    ssize_t linecap = 0;
    ssize_t linelen;
    linelen = getline(&line, &linecap, fp);
    if(linelen!=-1){
        while(linelen>0 && (line[linelen-1]=='\n' || line[linelen-1] =='\r')){
            linelen--;
        }
    
        E.row.size = linelen;
        E.row.chars = malloc(linelen + 1);
        memcmp(E.row.chars, line, linelen);
        E.row.chars[linelen] = '\0';
        E.numrows = 1;
    }
    free(line);
    fclose(fp);
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

void editorProcessKeypress(){
    int c = editorReadKey();

    //Break se der ctrl q
    switch(c){
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        //only implementing page up and down to help with the scroll
        case PAGE_UP:
        case PAGE_DOWN:
        {
            int count = E.screenrows;
            while(count--){
                editorMoveCursor(c==PAGE_UP? ARROW_UP: ARROW_DOWN);
            }
        }
        break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

void editorMoveCursor(__int64_t key){
    switch(key){
        case ARROW_LEFT:
            if(E.cx!=0) E.cx--;
            break; 
        case ARROW_RIGHT:
            if(E.cx != E.screencols -1) E.cx++;
            break;
        case ARROW_UP:
            if(E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy != E.screenrows-1) E.cy++;
            break;
    }
}

/* OUTPUT */

void editorDrawRows(abuf *ab){
    for(int i=0;i<E.screenrows;i++){
        if(i>=E.numrows){ 
            if(i == E.screenrows/3){
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                "Text Editor -- version %s", TE_VERSION);
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
            int len = E.row.size;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, E.row.chars, len);
        }
        abAppend(ab, "\x1b[K", 3); //dar clear linha por linha ao inves da tela inteira       
        if(i<E.screenrows-1){
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshSCreen(){
    //"\x1b" is a escape character
    //We are sending a escape sequence to clear the entire screen
    //[2 (clear entire screen), [1 untill the cursor, [0 from the cursor to the end 
    abuf ab = ABUF_INIT;
    
    abAppend(&ab, "\x1b[?25l", 6); //Hide the cursor
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    //abAppend(&ab, "\x1b[H", 3);
    abAppend(&ab, "\x1b[?25h", 6); //Show the cursor
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/* INIT */

//Init all the fields in the E struct
void initEditor(){
    E.cx = E.cy = E.numrows = 0;
    if(getWindowSize(&E.screenrows, &E.screencols) == -1){
        die("getWindowSize");
    }
}

int main(int argc, char **argv){

    enableRawMode();
    initEditor();
    if(argc>=2)editorOpen(argv[1]);

    char c;
    while (1) {
        editorRefreshSCreen();
        editorProcessKeypress();
    }
    return 0;
}