/*
Marcelo Gomes 2021222994
Pedro Brites 2021226319
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/stat.h>

int fd;
int count;

void clean_resources(){

    printf("\nCLEANING RESOURCES...\n");
    close(fd);

}

void sigtstp_handler(int signum) {
    printf("SIGNAL SIGTSTP RECEIVED\n");
    printf("\nNUMBER OF MESSAGES SENT: %d\n\n", count);
}

void sigint_handler(int signum){
    printf("SIGNAL SIGINT RECEIVED\n");
    clean_resources();
    printf("CLOSING\n");
    exit(0);
}

int isint(const char *str){
    while (*str != '\0'){
        if(!isdigit(*str)) return 1;
        ++str;
    }
    return 0;
}

int isint_neg(const char *str) {

    if (*str == '-') {
        ++str;
    }

    if (!isdigit(*str)) {
        return 1;
    }

    while (*str != '\0') {
        if (!isdigit(*str)) {
            return 1;
        }
        ++str;
    }

    return 2;
}

int main(int argc, char const *argv[])
{
    if (argc < 6) {
        printf("sensor {identificador do sensor} {intervalo entre envios em segundos (>=0)} {chave} {valor inteiro mínimo a ser enviado} {valor inteiro máximo a ser enviado}\n");
        exit(-1);
    }

    struct sigaction sa_int, sa_tstp;

    sigemptyset(&sa_int.sa_mask);
    sigaddset(&sa_int.sa_mask, SIGINT);
    sa_int.sa_flags = 0;
    sa_int.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa_int, NULL);
    sigemptyset(&sa_tstp.sa_mask);
    sigaddset(&sa_tstp.sa_mask, SIGTSTP);
    sa_tstp.sa_flags = 0;
    sa_tstp.sa_handler = sigtstp_handler;
    sigaction(SIGTSTP, &sa_tstp, NULL);

    //verifica o id do sensor
    if (strlen(argv[1]) < 3) {
        printf("ID HAS TO HAVE AT LEAST 3 CHARACTERS\n");
        clean_resources();
        return 1;
    }

    if (strlen(argv[1]) > 32) {
        printf("ID HAS TO HAVE 32 CHARACTERS MAX\n");
        clean_resources();
        return 1;
    }

    //verifica a chave
    if (strlen(argv[3]) < 3) {
        printf("KEY HAS TO HAVE AT LEAST 3 CHARACTERS\n");
        clean_resources();
        return 1;
    }

    if (strlen(argv[3]) > 32) {
        printf("KEY HAS TO HAVE 32 CHARACTERS MAX\n");
        clean_resources();
        return 1;
    }
    
    for (int i = 0; i < strlen(argv[3]); i++) {
        if (!isdigit(argv[3][i]) && !isalpha(argv[3][i]) && argv[3][i] != '_') {
            printf("KEY HAS AN INVALID CHARACTER\n");
            clean_resources();
            return 1;
        }
    }

    //verifica inteiros
    if(isint(argv[2]) == 1){
        printf("INTERVAL HAS TO BE INT\n");
        clean_resources();
        return 1;
    }
    
    if (isint_neg(argv[4]) != 2) {
    	printf("MIN VALUE HAS TO BE INT\n");
    	clean_resources();
    	return 1;
	}

    if(isint_neg(argv[5]) == 1){
        printf("MAX VALUE HAS TO BE INT\n");
        clean_resources();
        return 1;
    }

    if(atoi(argv[5]) <= atoi(argv[4])){
        printf("MAX VALUE HAS TO BE GREATER THAN MIN\n");
        clean_resources();
        return 1;
    }

	srand(time(NULL));
	
    //escreve para o named pipe
    char buffer[100];
    while(1){
        //abre named pipe 'SENSOR_PIPE' para escrever
	
    	if ((fd = open("SENSOR_PIPE", O_WRONLY)) == -1){
        	printf("COULD NOT OPEN 'SENSOR_PIPE' FOR WRITING\n");
        	return 1;
    	}
        snprintf(buffer, sizeof(buffer), "%s#%s#%d", argv[1], argv[3], rand() % (atoi(argv[5]) - atoi(argv[4])) + atoi(argv[4]));
        printf("%s\n", buffer);
        if (write(fd, &buffer, sizeof(buffer)) < 0){
            printf("ERROR WRITING TO NAMED PIPE\n");
            clean_resources();
            return 1;
        }
        count++;
        sleep(atoi(argv[2]));
        close(fd);
    }

    return 0;
}