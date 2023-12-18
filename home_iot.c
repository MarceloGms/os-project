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
#include <errno.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/msg.h>
#include <sys/ipc.h>

#define MAX 100
#define MESSAGE_QUEUE_KEY 123456

FILE *flog;
sem_t *log_sem, *shm_sem;
sem_t semEmpty, semFull;
pthread_mutex_t mutexIQ, mutexWORK;
pthread_cond_t condWORK;
char **INTERNAL_QUEUE;
int count_int = 0;
pthread_t sens_reader, cons_reader, dispatcher;
int shm_id;
int fd_sens, fd_cons, (*fd)[2];
int N_PROCESSES;
int n_workers, queue_sz, max_keys, max_sensors, max_alerts;
int msgid;

struct message_buffer {
    long msg_type;
  char data[200];
};

struct message_buffer m;

typedef struct{
    char key[32];
    int total_som, last, min, max, count;
    double avg;
}Stat_struct;

typedef struct{
    char id[33], key[32];
}Sensor_struct;

typedef struct{
    char id[33], key[32];
    int min, max, user;
}Alert_struct;

typedef struct{
    Sensor_struct *sens;
    Alert_struct *alerts;
    Stat_struct *stats;
    int *worker_state;
    int available_workers;
    int n_sens;
    int n_alerts;
    int n_keys;
    int end;
}Shm_struct;

Shm_struct * shm;

Sensor_struct null_sens = { "", ""};
Alert_struct null_alert = { "", "", 0, 0, 0};
Stat_struct null_stat = {"", 0, 0, 0, 0, 0, 0.0};

void handle_inputs(int value, int limit){
    if(value < limit){
        printf("Valor inválido no ficheiro de configurações:%d (>=%d)\n", value, limit);
        exit(-1);
    }
}

void write_log(char *str, FILE *log){
    time_t rawtime;
    struct tm *timeinfo;
    char buff[MAX];

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buff, sizeof(buff), "%T", timeinfo);

    sem_wait(log_sem);

    fprintf(log, "%s %s\n", buff, str);
    fflush(log);
    printf("%s %s\n", buff, str);

    sem_post(log_sem);

}

void clean_resources(){
    
    pthread_cancel(sens_reader);
    pthread_join(sens_reader, NULL);

    pthread_cancel(cons_reader);
    pthread_join(cons_reader, NULL);

    pthread_cancel(dispatcher);
    pthread_join(dispatcher, NULL);
    
    shmdt(shm);
    shmctl(shm_id, IPC_RMID, NULL);

    pthread_mutex_destroy(&mutexIQ);
    pthread_mutex_destroy(&mutexWORK);
    pthread_cond_destroy(&condWORK);
    sem_destroy(&semEmpty);
    sem_destroy(&semFull);
    sem_close(shm_sem);
    sem_unlink("shm_sem");

    close(fd_cons);
    close(fd_sens);
    unlink("CONSOLE_PIPE");
    unlink("SENSOR_PIPE");

    for(int i = 0; i < N_PROCESSES; i++){
        wait(NULL);
    }

    for(int i = 0; i < n_workers; i++){
        close(fd[i][0]);
        close(fd[i][1]);
    }
    
    msgctl(msgid, IPC_RMID, NULL);
}


void sigint_handler(int signum){
    shm->end = 1;
    for (int i = 0; i < n_workers; i++){
        close(fd[i][0]);
        write(fd[i][1], "q", strlen("q"));
    }
    write_log("SIGNAL SIGINT RECEIVED", flog);
    write_log("HOME_IOT SIMULATOR WAITING FOR LAST TASKS TO FINISH...", flog);
    clean_resources();
    write_log("HOME_IOT SIMULATOR CLOSING", flog);
    sem_unlink("log_sem");
    sem_close(log_sem);
    fclose(flog);
    exit(0);
}

//funções das threads
void* rout_sens(){
	fd_set read_set_sens;
    char texto_sens[100], texto_sen[200], full[150];
    
    write_log("THREAD SENSOR_READER CREATED", flog);
	
    while(shm->end != 1){
    	FD_ZERO(&read_set_sens);
    	FD_SET(fd_sens, &read_set_sens);
    	if (select(fd_sens+1, &read_set_sens, NULL, NULL, NULL) > 0){
    		if (read(fd_sens, texto_sens, 100) == -1){
        		printf("Erro a receber do Sensor_pipe");
        		exit(1);
    		}
    		if (sem_trywait(&semEmpty) == 0) {
    		    pthread_mutex_lock(&mutexIQ); 
    			snprintf(texto_sen, sizeof(texto_sen), "sensor#%s", texto_sens);
        		INTERNAL_QUEUE[count_int] = texto_sen;
        		count_int++;
    	        pthread_mutex_unlock(&mutexIQ);
                sem_post(&semFull);
    		}else{
        		//Pedido não é efetuado devido à internal queue estar cheia
        		snprintf(full, sizeof(full), "INTERNAL_QUEUE IS FULL => DELETING %s", texto_sens);
        		write_log(full, flog);
    		}
    	}
    }
    pthread_exit(NULL);
    return NULL;
}

void* rout_cons(){
	fd_set read_set_cons;
    char texto_cons[250];
    
    write_log("THREAD CONSOLE_READER CREATED", flog);

    while(shm->end != 1){
    	FD_ZERO(&read_set_cons);
    	FD_SET(fd_cons, &read_set_cons);
    	if (select(fd_cons+1, &read_set_cons, NULL, NULL, NULL) > 0){
    		if (read(fd_cons, texto_cons, 250) == -1){
        		printf("Erro a receber do Sensor_pipe");
        		exit(1);
    		}
            sem_wait(&semEmpty);
            pthread_mutex_lock(&mutexIQ);                                
        	INTERNAL_QUEUE[count_int] = texto_cons;
        	count_int++;
            pthread_mutex_unlock(&mutexIQ);
            sem_post(&semFull);
    	}
    }
    pthread_exit(NULL);
    return NULL;
}

void* rout_disp(){
    char inicio[200], funcao[100];
    
    write_log("THREAD DISPATCHER CREATED", flog);
    
    while(shm->end != 1){
        int con = 0;
        if (strcmp(INTERNAL_QUEUE[0], " ") == 0){
            
        }else{
            for (int i = 0; i < queue_sz; i++){
                strcpy(inicio, INTERNAL_QUEUE[i]);
                strcpy(inicio, strtok(inicio, "#"));
                if (strcmp(inicio, "console") == 0){
                    sem_wait(&semFull);
                    pthread_mutex_lock(&mutexIQ);
                    con = 1;
                    strcpy(funcao, INTERNAL_QUEUE[i]);
                    for (int j = i; j < queue_sz -1; j++){
                        INTERNAL_QUEUE[j] = INTERNAL_QUEUE[j+1];
                        if (j == queue_sz-1){
                            INTERNAL_QUEUE[j] = " ";
                        }
                    }
                    count_int--;
                    pthread_mutex_unlock(&mutexIQ);
                    sem_post(&semEmpty);
                    break;
                }
            }
            if (con == 0){
                sem_wait(&semFull);
                pthread_mutex_lock(&mutexIQ);
                strcpy(funcao, INTERNAL_QUEUE[0]);
                for (int h = 0; h < queue_sz - 1; h++){
                    INTERNAL_QUEUE[h] = INTERNAL_QUEUE[h+1];
                    if (h == queue_sz-1){
                        INTERNAL_QUEUE[h] = " ";
                    }
                }
                count_int--;
                pthread_mutex_unlock(&mutexIQ);
                sem_post(&semEmpty);
            }
            pthread_mutex_lock(&mutexWORK);
            while(shm->available_workers == 0){
                pthread_cond_wait(&condWORK, &mutexWORK);
            }
            for (int i = 0; i < n_workers; i++){
            	if (shm->worker_state[i] == 0){
            		char buf[100], func[100];
            		char *texto;
            		strcpy(func, funcao);
            		if (strcmp((texto = strtok(funcao, "#")), "console") == 0){
            			char *cmd = strtok(NULL, "#");
            			
            			if (strcmp(cmd, "stats") == 0){
        					snprintf(buf, sizeof(buf), "DISPATCHER: SHOW STATS SENT FOR PROCESSING ON WORKER %d", i+1);
        					write_log(buf, flog);
			
            			}else if(strcmp(cmd, "sensors") == 0){
                			snprintf(buf, sizeof(buf), "DISPATCHER: SHOW SENSORS SENT FOR PROCESSING ON WORKER %d", i+1);
        					write_log(buf, flog);
                			
            			}else if(strcmp(cmd, "list_alerts") == 0){
                			snprintf(buf, sizeof(buf), "DISPATCHER: LIST ALERTS SENT FOR PROCESSING ON WORKER %d", i+1);
        					write_log(buf, flog);
                			
            			}else if(strcmp(cmd, "reset") == 0){
                			snprintf(buf, sizeof(buf), "DISPATCHER: RESET STATS SENT FOR PROCESSING ON WORKER %d", i+1);
        					write_log(buf, flog);
			
            			}else if(strcmp(cmd, "add_alert") == 0){
                			char *id_alert = strtok(NULL, "#");
                			char *chave_alert = strtok(NULL, "#");
                			int min_alert = atoi(strtok(NULL, "#"));
                			int max_alert = atoi(strtok(NULL, "\0"));
                			snprintf(buf, sizeof(buf), "DISPATCHER: ADD ALERT %s (%s %d TO %d) SENT FOR PROCESSING ON WORKER %d", id_alert, chave_alert, min_alert, max_alert, i+1);
        					write_log(buf, flog);
                			
            			}else if(strcmp(cmd, "remove_alert") == 0){
                			char *id_alert = strtok(NULL, "\0");
                			snprintf(buf, sizeof(buf), "DISPATCHER: REMOVE ALERT %s SENT FOR PROCESSING ON WORKER %d", id_alert, i+1);
        					write_log(buf, flog);
                		}
        			}else{
        				char *sens_id = strtok(NULL, "#");
            			char *chave = strtok(NULL, "#");
            			
            			snprintf(buf, sizeof(buf), "DISPATCHER: %s DATA (FROM %s SENSOR) SENT FOR PROCESSING ON WORKER %d", chave, sens_id, i+1);
        				write_log(buf, flog);
            		}
            		//write no pipe
            		close(fd[i][0]);
            		write(fd[i][1], func, strlen(func));
            		break;
            	}
            }
            pthread_mutex_unlock(&mutexWORK);
        }
    }
    pthread_exit(NULL);
    return NULL;
}

//processos worker e alert_watcher

void worker(int pipe_number){
    char resp[MAX];
    char buf[MAX];
    snprintf(buf, sizeof(buf), "WORKER %d READY", pipe_number+1);
    write_log(buf, flog);
    close(fd[pipe_number][1]);
    while(shm->end != 1){
        ssize_t bytesRead = read(fd[pipe_number][0], resp, 100 * sizeof(char));
        resp[bytesRead] = '\0';
        if(strcmp(resp, "q") == 0) break;
        sem_wait(shm_sem);
        shm->worker_state[pipe_number] = 1;
        shm->available_workers--;
        char *texto;
        if (strcmp((texto = strtok(resp, "#")), "console") == 0){
            int ident = atoi(strtok(NULL, "#"));
            m.msg_type = ident;
            char *cmd = strtok(NULL, "#");
            if (strcmp(cmd, "stats") == 0){
                for (int i = 0; i < shm->n_keys; i++){
                	if (strcmp(shm->sens[i].key, "") != 0){
                    	snprintf(m.data, sizeof(m.data), "%s %d %d  %d  %.2f  %d", shm->stats[i].key, shm->stats[i].last, shm->stats[i].min, shm->stats[i].max, shm->stats[i].avg, shm->stats[i].count);
                    	msgsnd(msgid, &m, sizeof(m) - sizeof(long) - sizeof(long), 0);
                    }
                }
                strcpy(m.data, "end");
                msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                snprintf(buf, sizeof(buf), "WORKER%d: SHOW STATS PROCESSING COMPLETED", pipe_number+1);
        		write_log(buf, flog);

            }else if(strcmp(cmd, "sensors") == 0){
                for (int i = 0; i < shm->n_sens; i++){
                	if (strcmp(shm->sens[i].id, "") != 0){
                    	snprintf(m.data, sizeof(m.data), "%s", shm->sens[i].id);
                    	msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                    }
                }
                strcpy(m.data, "end");
                msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                snprintf(buf, sizeof(buf), "WORKER%d: SHOW SENSORS PROCESSING COMPLETED", pipe_number+1);
        		write_log(buf, flog);
                
            }else if(strcmp(cmd, "list_alerts") == 0){
                for (int i = 0; i < shm->n_alerts; i++){
                	if (strcmp(shm->alerts[i].id, "") != 0){
                    	snprintf(m.data, sizeof(m.data), "%s %s %d %d", shm->alerts[i].id, shm->alerts[i].key, shm->alerts[i].min, shm->alerts[i].max);
                    	msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                    }
                }
                strcpy(m.data, "end");
                msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                snprintf(buf, sizeof(buf), "WORKER%d: LIST ALERTS PROCESSING COMPLETED", pipe_number+1);
        		write_log(buf, flog);
                
            }else if(strcmp(cmd, "reset") == 0){
                for (int i = 0; i < shm->n_keys; i++){
                    shm->stats[i] = null_stat;
                }
                shm->n_keys = 0;
                for (int i = 0; i < shm->n_sens; i++){
                    shm->sens[i] = null_sens;
                }
                shm->n_sens = 0;
                
                strcpy(m.data, "OK");
                msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                snprintf(buf, sizeof(buf), "WORKER%d: RESET STATS PROCESSING COMPLETED", pipe_number+1);
        		write_log(buf, flog);

            }else if(strcmp(cmd, "add_alert") == 0){
                char *id_alert = strtok(NULL, "#");
                char *chave_alert = strtok(NULL, "#");
                int min_alert = atoi(strtok(NULL, "#"));
                int max_alert = atoi(strtok(NULL, "\0"));
                int alert_exists = 0;
                for (int i = 0; i < max_alerts; i++){
                    if (strcmp(id_alert, shm->alerts[i].id) == 0){
                        alert_exists = 1;
                        strcpy(shm->alerts[i].key, chave_alert);
                        shm->alerts[i].min = min_alert;
                        shm->alerts[i].max = max_alert;
                        shm->alerts[i].user = ident;
                        snprintf(buf, sizeof(buf), "WORKER%d: ADD ALERT %s (%s %d TO %d) PROCESSING COMPLETED", pipe_number+1, id_alert, chave_alert, min_alert, max_alert);
        				write_log(buf, flog);
                        break;
                    }
                }
        		
                if (alert_exists == 0){
                    if (shm->n_alerts == max_alerts){
                        strcpy(m.data, "ERROR");
                        msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                        write_log("MAX NUMBER OF ALERTS REACHED, DISCARDING OPERATION", flog);
                    }else{
                        for (int i = 0; i < max_alerts; i++){
                            if (strcmp(shm->alerts[i].id, "") == 0){
                                strcpy(shm->alerts[i].id, id_alert);
                                strcpy(shm->alerts[i].key, chave_alert);
                                shm->alerts[i].min = min_alert;
                                shm->alerts[i].max = max_alert;
                                shm->alerts[i].user = ident;
                                shm->n_alerts++;
                                break;
                            }
                        }
                        snprintf(buf, sizeof(buf), "WORKER%d: ADD ALERT %s (%s %d TO %d) PROCESSING COMPLETED", pipe_number+1, id_alert, chave_alert, min_alert, max_alert);
        				write_log(buf, flog);
                    }
                }
                strcpy(m.data, "OK");
                msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                  
            }else if(strcmp(cmd, "remove_alert") == 0){
                char *id_alert = strtok(NULL, "\0");
                int alert_exists = 0;
                for (int i = 0; i < max_alerts; i++){
                    if (strcmp(id_alert, shm->alerts[i].id) == 0){
                        alert_exists = 1;
                        shm->alerts[i] = null_alert;
                        for (int j = i; j < max_alerts - 1; j++) shm->alerts[j] = shm->alerts[j+1];
                        shm->n_alerts--;
                        strcpy(m.data, "OK");
                        msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                        
                        snprintf(buf, sizeof(buf), "WORKER%d: REMOVE ALERT %s PROCESSING COMPLETED", pipe_number+1, id_alert);
        				write_log(buf, flog);
                        break;
                    }
                }
                if (alert_exists == 0){
                    strcpy(m.data, "ERROR");
                    msgsnd(msgid, &m, sizeof(m) - sizeof(long), 0);
                    
                    snprintf(buf, sizeof(buf), "WORKER%d: THERE IS NO ALERT WITH THAT ID", pipe_number+1);
        			write_log(buf, flog);
                }
            }else{
            	snprintf(buf, sizeof(buf), "WORKER%d: CONSOLE COMMAND ERROR", pipe_number+1);
        		write_log(buf, flog);
            }
        }else{
        	char *sens_id = strtok(NULL, "#");
            char *chave = strtok(NULL, "#");
            int val = atoi(strtok(NULL, "\0"));
            
            int exists = 0;
            for (int i = 0; i < shm->n_keys; i++){
                if (strcmp(chave, shm->stats[i].key) == 0){
                    exists = 1;
                    shm->stats[i].last = val;
                    shm->stats[i].total_som = shm->stats[i].total_som + val;
                    if (val > shm->stats[i].max) shm->stats[i].max = val;
                    else if(val < shm->stats[i].min) shm->stats[i].min = val;
                    shm->stats[i].count++;
                    shm->stats[i].avg = (double) shm->stats[i].total_som / (double) shm->stats[i].count;
                    break;
                }
            }
            if (exists == 0){
                if (shm->n_keys == max_keys){
                    write_log("MAX NUMBER OF KEYS REACHED, DISCARDING OPERATION", flog);
                }else{
                    for (int i = 0; i < max_keys; i++){
                        if (strcmp(shm->stats[i].key, "") == 0){
                            shm->n_keys++;
                            strcpy(shm->stats[i].key, chave);
                            shm->stats[i].count = 1;
                            shm->stats[i].last = val;
                            shm->stats[i].min = val;
                            shm->stats[i].max = val;
                            shm->stats[i].avg = (double) val;
                            shm->stats[i].total_som = val;
                            break;
                        }
                    }
                }
            }
            int sens_exists = 0;
            for (int i = 0; i < shm->n_sens; i++){
                if (strcmp(sens_id, shm->sens[i].id) == 0){
                    sens_exists = 1;
                    break;
                }
            }
            if (sens_exists == 0){
                if (shm->n_sens == max_sensors){
                    write_log("MAX NUMBER OF SENSORS REACHED, DISCARDING OPERATION", flog);
                }else{
                    for (int i = 0; i < max_sensors; i++){
                        if (strcmp(shm->sens[i].id, "") == 0){
                            strcpy(shm->sens[i].id, sens_id);
                            strcpy(shm->sens[i].key, chave);
                            shm->n_sens++;
                            break;
                        }
                    }
                }
            }
            snprintf(buf, sizeof(buf), "WORKER%d: %s DATA (FROM %s SENSOR) PROCESSING COMPLETED", pipe_number+1, chave, sens_id);
        	write_log(buf, flog);
        }
        shm->worker_state[pipe_number] = 0;
        shm->available_workers++;
        pthread_cond_signal(&condWORK);
        sem_post(shm_sem);
    }
    exit(0);
}

void alerts_watcher(){
	int trg = 100000;
    while (shm->end != 1){
        sem_wait(shm_sem);
        for (int i = 0; i < shm->n_keys; i++){
            for (int j = 0; j < shm->n_alerts; j++){
                if (strcmp(shm->alerts[j].key, shm->stats[i].key) == 0){
                    if (shm->stats[i].last < shm->alerts[j].min || shm->stats[i].last > shm->alerts[j].max){
                    	if (shm->stats[i].last != trg){
                            m.msg_type = shm->alerts[j].user;
                        	snprintf(m.data, sizeof(m.data), "ALERT %s (%s %d TO %d) TRIGGERED", shm->alerts[j].id, shm->alerts[j].key, shm->alerts[j].min, shm->alerts[j].max);
                        	msgsnd(msgid, &m, sizeof(m), 0);
                        	write_log(m.data, flog);
                        	trg = shm->stats[i].last;
                        }
                    }
                }
            }
        }
        sem_post(shm_sem);
    }
    exit(0);
}


int main(int argc, char const *argv[]){

    if (argc < 2) {
        printf("home_iot {ficheiro de configuração}\n");
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
    sa_tstp.sa_handler = SIG_IGN;
    sigaction(SIGTSTP, &sa_tstp, NULL);

    FILE *fp;
    fp = fopen(argv[1], "r");
    flog = fopen("log.txt", "w");
    
    if(fp == NULL || flog == NULL){
        printf("erro ao abrir ficheiro\n");
        return 1;
    }
    
    if (fscanf(fp, "%d", &queue_sz) == 0) {
        printf("valor invalido no ficheiro config\n");
        return 1;
    }
    handle_inputs(queue_sz, 1);
    
    if (fscanf(fp, "%d", &n_workers) == 0) {
        printf("valor invalido no ficheiro config\n");
        return 1;
    }
    handle_inputs(n_workers, 1);
    
    if (fscanf(fp, "%d", &max_keys) == 0) {
        printf("valor invalido no ficheiro config\n");
        return 1;
    }
    handle_inputs(max_keys, 1);
    
    if (fscanf(fp, "%d", &max_sensors) == 0){ 
        printf("valor invalido no ficheiro config\n");
        return 1;
    }
    handle_inputs(max_sensors, 1);

    if (fscanf(fp, "%d", &max_alerts) == 0){
         printf("valor invalido no ficheiro config\n");
         return 1;
    }
    handle_inputs(max_alerts, 0);
    
    fclose(fp);

    N_PROCESSES = n_workers+1;

    msgid = msgget(MESSAGE_QUEUE_KEY, 0666 | IPC_CREAT);
    if (msgid == -1){
    	perror("msgget");
        exit(1);
    }

	//inicialização de semaforos
	
   	if ((log_sem = sem_open("log_sem", O_CREAT|O_EXCL, 0700, 1)) == SEM_FAILED){
    	sem_unlink("log_sem");
    	log_sem = sem_open("log_sem", O_CREAT|O_EXCL, 0700, 1);
	}

    if ((shm_sem = sem_open("shm_sem", O_CREAT|O_EXCL, 0700, 1)) == SEM_FAILED){
    	sem_unlink("shm_sem");
    	shm_sem = sem_open("shm_sem", O_CREAT|O_EXCL, 0700, 1);
	}

    sem_init(&semEmpty, 0, queue_sz);
    sem_init(&semFull, 0, 0);

    write_log("HOME_IOT SIMULATOR STARTING", flog);

    //inicializa mutex e cond

    pthread_mutex_init(&mutexIQ, NULL);
    pthread_mutex_init(&mutexWORK, NULL);
    pthread_cond_init(&condWORK, NULL);

    //Alocar o espaço para o array INTERNAL_QUEUE

    INTERNAL_QUEUE = (char **)malloc(queue_sz * sizeof(char *));
    for (int i = 0; i < queue_sz; i++) {
        INTERNAL_QUEUE[i] = (char *)malloc(MAX * sizeof(char));
    }
    
    for (int i = 0; i < queue_sz; i++) {
        INTERNAL_QUEUE[i] = " ";
    }

    //criação dos unnamed pipes

    fd = malloc(n_workers * sizeof(int[2]));

    for (int i = 0; i < n_workers; ++i){
        if (pipe(fd[i]) == -1){
            printf("Ocorreu um erro ao abrir o pipe\n");
            return 1;
        }
    }

    //criação dos named pipes

	if (mkfifo("SENSOR_PIPE", 0777) == -1){
    	if(errno != EEXIST){
        	printf("O ficheiro nao pode ser criado\n");
        	return 1;
    	}
	}

    fd_sens = open("SENSOR_PIPE", O_RDWR);
    if (fd_sens == -1){
        printf("Erro ao abrir o Sensor_pipe\n");
    }
	
	if (mkfifo("CONSOLE_PIPE", 0777) == -1){
    	if(errno != EEXIST){
        	printf("O ficheiro nao pode ser criado\n");
        	return 1;
    	}
	}

    fd_cons = open("CONSOLE_PIPE", O_RDWR);
    if (fd_cons == -1){
        printf("Erro ao abrir o Console_pipe\n");
    }

    //Criar a memória partilhada
    
    shm_id = shmget(IPC_PRIVATE, sizeof(Shm_struct) + (max_sensors * sizeof(Sensor_struct)) + (max_alerts * sizeof(Alert_struct)) + (max_keys * sizeof(Stat_struct)) + (n_workers * sizeof(int)), 0666|IPC_CREAT|IPC_EXCL);
    if (shm_id == -1){
    	printf("Erro na criacao da memoria partilhada");
    	return 1;
    }
    
    shm = (Shm_struct *) shmat(shm_id, NULL, 0);
    if (shm == (Shm_struct *) -1){
    	printf("Erro na adicao de memoria partilhada");
    	return 1;
    }

    shm->sens = (Sensor_struct *) ((char*) shm + sizeof(Shm_struct));
    shm->alerts = (Alert_struct *) ((char*) shm + sizeof(Shm_struct) + max_sensors*sizeof(Sensor_struct));
    shm->stats = (Stat_struct *) ((char*) shm + sizeof(Shm_struct) + max_sensors*sizeof(Sensor_struct) + max_alerts*sizeof(Alert_struct));
    shm->worker_state = (int *) ((char*) shm + sizeof(Shm_struct) + max_sensors*sizeof(Sensor_struct) + max_alerts*sizeof(Alert_struct) + max_keys*sizeof(Stat_struct));

    for (int i = 0; i < max_alerts; i++) shm->alerts[i] = null_alert;
    for (int i = 0; i < max_sensors; i++) shm->sens[i] = null_sens;
    for (int i = 0; i < max_keys; i++) shm->stats[i] = null_stat;
    for (int i = 0; i < n_workers; i++) shm->worker_state[i] = 0;

    shm->n_sens = 0;
    shm->n_alerts = 0;
    shm->n_keys = 0;
    shm->end = 0;
    shm->available_workers = n_workers;

    //criação dos processos worker e alerts watcher
    int pid;
    for(int i = 0; i < N_PROCESSES; ++i){
        if((pid = fork()) == 0){
            sigemptyset(&sa_int.sa_mask);
            sigaddset(&sa_int.sa_mask, SIGINT);
            sa_int.sa_flags = 0;
            sa_int.sa_handler = SIG_IGN;
            sigaction(SIGINT, &sa_int, NULL);
            if(i == n_workers){
                alerts_watcher();
            }
            else{
                worker(i);
            }
        }
        else if(pid == -1){
            printf("fork error\n");
            exit(1);
        }
    }

    //Cria as threads

    if (pthread_create(&sens_reader, NULL, &rout_sens, NULL) != 0){
        printf("Erro ao criar a thread Sensor_Reader");
        return 1;
    }

    if (pthread_create(&cons_reader, NULL, &rout_cons, NULL) != 0){
        printf("Erro ao criar a thread Console_Reader");
        return 1;
    }
    
    if (pthread_create(&dispatcher, NULL, &rout_disp, NULL) != 0){
        printf("Erro ao criar a thread Dispatcher");
        return 1;
    }
    
    while(wait(NULL) != -1 || errno != ECHILD);

    clean_resources();

    return 0;
}