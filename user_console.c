#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h> 
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <semaphore.h>

#define MAX 100
#define MESSAGE_QUEUE_KEY 123456

key_t key;
int msgid;
int fd;
int ident;
int pid;
sem_t *bin_sem;

struct message_buffer {
    long msg_type;
  	char data[200];
};

struct message_buffer m;

typedef struct {
    int flag;
} shared_flag;

int shm_id;
shared_flag* shm_ptr;

void clean_resources(){
	close(fd);
	sem_close(bin_sem);
    sem_unlink("bin_sem");
	waitpid(pid, NULL, 0);
	shmdt(shm_ptr);
    shmctl(shm_id, IPC_RMID, NULL);
}

void sigint_handler(int signum){
	shm_ptr->flag = 1;
    clean_resources();
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

void get_alert(){
	while(1){
		sem_wait(bin_sem);
		int ret = msgrcv(msgid, &m, sizeof(m), ident, IPC_NOWAIT);
		if (ret > 0) {
			printf("%s\n", m.data);
		}
		if (shm_ptr->flag) break;
		sem_post(bin_sem);
	}
	exit(0);
}

int main(int argc, char const *argv[])
{
    if (argc < 2) {
        printf("user_console {identificador da consola}\n");
        exit(-1);
    }

	shm_id = shmget(IPC_PRIVATE, sizeof(shared_flag), IPC_CREAT | 0666);
    if (shm_id < 0) {
        perror("shmget");
        exit(1);
    }

    shm_ptr = (shared_flag*) shmat(shm_id, NULL, 0);
    if (shm_ptr == (void*) -1) {
        perror("shmat");
        exit(1);
    }

    shm_ptr->flag = 0;

    struct sigaction sa_int, sa_tstp;

    sigemptyset(&sa_int.sa_mask);
    sigaddset(&sa_int.sa_mask, SIGINT);
    sa_int.sa_flags = 0;
    sa_int.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa_int, NULL);
    sigemptyset(&sa_tstp.sa_mask);
    sigaddset(&sa_tstp.sa_mask, SIGTSTP);
    sa_tstp.sa_flags = 0;
    sa_tstp.sa_handler = SIG_IGN;
    sigaction(SIGTSTP, &sa_tstp, NULL);
	
    msgid = msgget(MESSAGE_QUEUE_KEY, 0666);

	if ((bin_sem = sem_open("bin_sem", O_CREAT|O_EXCL, 0700, 1)) == SEM_FAILED){
    	sem_unlink("bin_sem");
    	bin_sem = sem_open("bin_sem", O_CREAT|O_EXCL, 0700, 1);
	}

	if (isint(argv[1]) != 0) {
		printf("id has to be int\n");
		return 1;
	}
    ident = atoi(argv[1]);
    //printf("\n%d\n", ident);
	if (ident < 0){
		printf("id < 0\n");
		return 1;
	}

	//abre named pipe 'CONSOLE_PIPE' para escrever
	if ((fd = open("CONSOLE_PIPE", O_WRONLY)) == -1){
		printf("COULD NOT OPEN 'CONSOLE_PIPE' FOR WRITING\n");
		return 1;
	}
	
	if((pid = fork()) == 0){
		get_alert();
	}else if(pid == -1){
		printf("fork error\n");
		exit(1);
	}

    while(1){
        char str[MAX];
        char comando[MAX];

        fgets(str, MAX, stdin);
        str[strcspn(str, "\n")] = '\0';
        strcpy(comando, strtok(str, " "));

        if (strcmp(comando, "exit") == 0) {
			shm_ptr->flag = 1;
            clean_resources();
            break;
        } else if (strcmp(comando, "stats") == 0) {
			sem_wait(bin_sem);
			char text[200];
			snprintf(text, sizeof(text), "console#%d#stats", ident);
    		if (write(fd, text, sizeof(text)) < 0){
            	printf("ERROR WRITING TO NAMED PIPE\n");
            	clean_resources();
            	return 1;
        	}
            printf("Key   Last Min Max Avg  Count\n");
            while (1){
				if (msgrcv(msgid, &m, sizeof(m)- sizeof(long), ident, 0) == EOF) continue;
				if (strcmp(m.data, "end") == 0) {
					break;
				}
				printf("%s\n", m.data);
			}
			sem_post(bin_sem);
        } else if (strcmp(comando, "reset") == 0) {
			sem_wait(bin_sem);
			char text[250];
			snprintf(text, sizeof(text), "console#%d#reset", ident);
    		if (write(fd, text, sizeof(text)) < 0){
            	printf("ERROR WRITING TO NAMED PIPE\n");
            	clean_resources();
            	return 1;
        	}
			msgrcv(msgid, &m, sizeof(m) - sizeof(long), ident, 0);
            printf("%s\n", m.data);
			sem_post(bin_sem);
        } else if (strcmp(comando, "sensors") == 0) {
			sem_wait(bin_sem);
			char text[200];
			snprintf(text, sizeof(text), "console#%d#sensors", ident);
    		if (write(fd, text, sizeof(text)) < 0){
            	printf("ERROR WRITING TO NAMED PIPE\n");
            	clean_resources();
            	return 1;
        	}
            printf("ID\n");
            while (1){
            	if (msgrcv(msgid, &m, sizeof(m)- sizeof(long), ident, 0) == EOF) continue;
				if (strcmp(m.data, "end") == 0) {
					break;
				}
				printf("%s\n", m.data);
			}
			sem_post(bin_sem);
        } else if (strcmp(comando, "add_alert") == 0) {
            char id[33], chave[MAX], min[16], max[16];
            int a = 0;
            char *temp;

            //verifica se foram escritos todos os parametros necessários

            if((temp = strtok(NULL, " ")) == NULL){
            	printf("add_alert {id} {chave} {min} {max}\n");
            }else{
            	strcpy(id, temp);
            	if((temp = strtok(NULL, " ")) == NULL){
            		printf("add_alert {id} {chave} {min} {max}\n");
            	}else{
            		strcpy(chave, temp);
            		if((temp = strtok(NULL, " ")) == NULL){
            			printf("add_alert {id} {chave} {min} {max}\n");
            		}else{
            			strcpy(min, temp);
            			if((temp = strtok(NULL, " ")) == NULL){
            				printf("add_alert {id} {chave} {min} {max}\n");
            			}else{
            				strcpy(max, temp);
            				for (int i = 0; i < strlen(id); i++) {            //verifica se o id é alfanumérico
                				if (!isdigit(id[i]) && !isalpha(id[i])) {
                    				a = 1;
                				}
                			}

                            //executa todas as verificações do formato do texto escrito pelo utilizador

            				if (strlen(id) >= 3 && strlen(id) <= 32){
                				if(a == 0){
                    				if (isint_neg(min) == 2 && isint_neg(max) == 2){
                        				if (atoi(max) > atoi(min)){
											sem_wait(bin_sem);
                        					char texto[250];
                            				snprintf(texto, sizeof(texto), "console#%d#add_alert#%s#%s#%s#%s", ident, id, chave, min, max);
                            				//mandar o alert criado para o System Manager
    										
    										if (write(fd, texto, sizeof(texto)) < 0){
            									printf("ERROR WRITING TO NAMED PIPE\n");
            									clean_resources();
            									return 1;
        									}
											msgrcv(msgid, &m, sizeof(m) - sizeof(long), ident, 0);
                            				printf("%s\n", m.data);
											sem_post(bin_sem);
                        				}else{
                            				printf("O numero maximo tem de ser maior que o numero minimo\n");
                        				}
                    				}else{
                        				printf("Os numeros minimo e maximo tem de ser inteiros\n");
                    				}
                				}else{
                    				printf("O id so pode ter caracteres alfanumericos\n");
                				}
            				}else{
                				printf("O id tem de ter entre 3 e 32 caracteres alfanumericos\n");
            				}
            			}
            		}
            	}
            }
        } else if (strcmp(comando, "remove_alert") == 0) {
            char alert_id[33];
            int a = 0;
            char *temp;
            if((temp = strtok(NULL, "\0")) == NULL){           //verifica se foi escrito o parâmetro necessário
                printf("remove_alert {id}\n");
            }else{
            	strcpy(alert_id, temp);
            	for (int i = 0; i < strlen(alert_id); i++) {
                	if (!isdigit(alert_id[i]) && !isalpha(alert_id[i])) {
                    	a = 1;
                	}
            	}
            	if (strlen(alert_id) >= 3 && strlen(alert_id) <= 32){
                	if(a == 0){
						sem_wait(bin_sem);
                		char texto[250];
                        snprintf(texto, sizeof(texto), "console#%d#remove_alert#%s", ident, alert_id);
    					if (write(fd, texto, sizeof(texto)) < 0){
            				printf("ERROR WRITING TO NAMED PIPE\n");
            				clean_resources();
            				return 1;
        				}
        				msgrcv(msgid, &m, sizeof(m) - sizeof(long), ident, 0);
            			printf("%s\n", m.data);
						sem_post(bin_sem);
            			
                	}else printf("O id so pode ter caracteres alfanumericos\n");

            	}else printf("O id tem de ter entre 3 e 32 caracteres alfanumericos\n");
            }
        } else if (strcmp(comando, "list_alerts") == 0) {
			sem_wait(bin_sem);
			char text[250];
			snprintf(text, sizeof(text), "console#%d#list_alerts", ident);
			if (write(fd, text, sizeof(text)) < 0){
    			printf("ERROR WRITING TO NAMED PIPE\n");
    			clean_resources();
    			return 1;
			}
            printf("ID  Key   MIN MAX\n");
			while (1) {
				if (msgrcv(msgid, &m, sizeof(m)- sizeof(long), ident, 0) == EOF) continue;
				if (strcmp(m.data, "end") == 0) {
					break;
				}
				printf("%s\n", m.data);
			}
			sem_post(bin_sem);
        } else {
            printf("comando inválido\n");
        }
    }

    return 0;

}
