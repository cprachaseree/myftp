#include "myftp.h"
#include <pthread.h>

void check_arg(int argc);
void read_serverconfig(char *serverconfig_name, int *n, int *k, int *block_size, int *server_id, int *PORT_NUMBER);
int check_port_num(int arg_num, char *port_number_string);
void* connection(void* client_sd);
void check_arg(int argc);
void list(int client_sd);
void get_file(int client_sd, int file_name_length);
void put_file(int client_sd, int file_name_length);
void store_metadata(int file_size, char *file_name, int file_name_length);

int n, k, block_size, server_id, PORT_NUMBER;

int main(int argc, char *argv[]) {
    check_arg(argc);
    
    read_serverconfig(argv[1], &n, &k, &block_size, &server_id, &PORT_NUMBER);
	// ceate dir metadata if not existed
	if (opendir("./data/metadata") == NULL) {
		mkdir("./data/metadata", 0777);
	}
    // open socket
	int sd = socket(AF_INET, SOCK_STREAM, 0);
	if (sd < 0) {
        printf("open socket failed: %s (Errno: %d)\n", strerror(errno), errno);
        return -1;
	}
    // bind port to socket
	struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(PORT_NUMBER);
    if(bind(sd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
		printf("bind failed: %s (Errno: %d)\n", strerror(errno), errno);
		return -1;
	}
    if(listen(sd, 3) < 0) {
		printf("listen failed: %s (Errno: %d)\n",strerror(errno), errno);
		return -1;
	}
    printf("Server is listening to connections\n");
    pthread_t threads[10];
    int thread_count = 0;
    while(1) {
        // keep accepting new connections
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        int* client_sd = malloc(sizeof(int));
	    if((*client_sd = accept(sd, (struct sockaddr *) &client_addr, &addr_len))<0){
		    printf("accept failed: %s (Errno: %d)\n", strerror(errno), errno);
            return -1;
	    }
        // creating thread for transmission stage to support concurrent clients
        pthread_create(&threads[thread_count], NULL, &connection, (void*) client_sd);
        thread_count++;
    }
}

void check_arg(int argc) {
    if (argc != 2) {
    print_arg_error("server");
  }
}

void read_serverconfig(char *serverconfig_name, int *n, int *k, int *block_size, int *server_id, int *PORT_NUMBER) {
    FILE* serverconfig_fp;
    if ((serverconfig_fp = fopen(serverconfig_name, "r")) == NULL) {
        printf("Unable to open file %s\n", serverconfig_name);
        print_arg_error("server");
        exit(0);
    }
    if (fscanf(serverconfig_fp, "%d\n%d\n%d\n%d\n%d\n", n, k,  server_id, block_size, PORT_NUMBER) != 5) {
        printf("Error in serverconfig.txt.\n");
        exit(0);
    }
    printf("n=%d\nk=%d\nblock_size=%d\nserver_id=%d\nPORT_NUMBER=%d\n", *n, *k, *block_size, *server_id, *PORT_NUMBER);
    fclose(serverconfig_fp);
}

void* connection(void* client_sd) {
    struct message_s client_request_message;
    memset(&client_request_message, 0, sizeof(client_request_message));
    int len;
    printf("Connected\n");
    fflush(stdout);
    if ((len = recv(*((int*) client_sd), &client_request_message, sizeof(client_request_message), MSG_WAITALL)) < 0) {
        printf("receive error: %s (Errno:%d)\n", strerror(errno),errno);
        exit(0);
    }
	int reply_length = ntohl(client_request_message.length);
    if (client_request_message.type == 0xA1) {
        printf("Received list request\n");
		list(*((int*) client_sd));
    } else if (client_request_message.type == 0xB1) {
        printf("Received get request\n");
        get_file(*((int*) client_sd), reply_length - sizeof(client_request_message));
    } else if (client_request_message.type == 0xC1) {
        printf("Received put request\n");
        put_file(*((int*) client_sd), reply_length - sizeof(client_request_message));
    }
}

void put_file(int client_sd, int file_name_length) {
    char *file_name = (char *) calloc(file_name_length, sizeof(char));
    int len, i;
    if ((len = recv(client_sd, file_name, file_name_length, MSG_WAITALL)) < 0) {
        printf("receive error: %s (Errno:%d)\n", strerror(errno),errno);
        exit(0);
    }
    printf("File name received is %s\n", file_name);
    struct message_s put_response;
    memset(&put_response, 0, sizeof(struct message_s));
    strcpy(put_response.protocol, "myftp");
    put_response.type = 0xC2;
    put_response.length = htonl(sizeof(put_response));
    if ((len = send(client_sd, &put_response, sizeof(struct message_s), 0)) < 0) {
        printf("Send Error: %s (Errno:%d)\n",strerror(errno),errno);
        exit(0);
    }
    // get data from myftpclient put()
    int file_size =  check_file_data_header(client_sd) - sizeof(struct message_s);
    send_file_header(client_sd, server_id);
    int num_of_blocks = check_file_data_header(client_sd) - sizeof(struct message_s);

    // store file size to metadata folder with same file name
    store_metadata(file_size, file_name, file_name_length);
    // store actual data into data/filename_stripeid
    // make file path name
    int serverid_digits, i_digits, file_path_length;
    char* buffer = (char *)calloc(block_size, sizeof(char));
    FILE *fp;
    
    for (i = 0; i < num_of_blocks; i++) {
        serverid_digits = snprintf(0,0,"%+d", server_id) - 1;
        i_digits = snprintf(0,0,"%+d", i) - 1;
        file_path_length = 5 + serverid_digits + 1 + ntohl(file_name_length) + 1 + i_digits;
        char *file_path = (char *) calloc(file_path_length, sizeof(char));
        snprintf(file_path, file_path_length, "data/%d_%s_%d", server_id, file_name, i);
        fp = fopen(file_path, "w");
        if ((len = recv(client_sd, buffer, block_size, MSG_WAITALL)) < 0) {
            printf("receive error: %s (Errno:%d)\n", strerror(errno),errno);
            exit(0);
        }
        fwrite(buffer, sizeof(buffer[0]), block_size, fp);
        fclose(fp);
    }
}

void get_file(int client_sd, int file_name_length) {
    int len, i;
    int file_size, num_of_stripes, serverid_digits, i_digits, file_path_length;
    char * buffer;
    FILE *fp;
    char *file_name = (char *) calloc(file_name_length, sizeof(char));
    if ((len = recv(client_sd, file_name, file_name_length, MSG_WAITALL)) < 0) {
        printf("receive error: %s (Errno:%d)\n", strerror(errno),errno);
        exit(0);
    }
    printf("File name received is %s\n", file_name);
    
    // create get reply
    struct message_s get_reply;
    memset(&get_reply, 0, sizeof(struct message_s));
    strcpy(get_reply.protocol, "myftp");
    char* file_path = (char *) calloc(14 + file_name_length, sizeof(char));
    strcpy(file_path, "data/metadata/");
    strcat(file_path, file_name);
    if (get_file_size(file_path) == -1) {
        printf("File does not exist\n");
        get_reply.type = 0xB3;
    } else {
        printf("File exists\n");
        get_reply.type = 0xB2;
    }
    get_reply.length = htonl(sizeof(struct message_s));
    // send get reply header
    if ((len = send(client_sd, &get_reply, sizeof(struct message_s), 0)) < 0) {
        printf("Send Error: %s (Errno:%d)\n",strerror(errno),errno);
        exit(0);
    }
    if (get_reply.type == 0xB3) {
        free(file_name);
        free(file_path);
        return;
    }
    FILE *mfp;
    mfp = fopen(file_path, "r");
    fscanf(mfp, "%d\n", &file_size);
    // send file size after getting it from metadata
    send_file_header(client_sd, file_size);
    // send serverid so client knows which column they will get
    send_file_header(client_sd, server_id);
    num_of_stripes = ((file_size - 1) / (k * block_size)) + 1;
    // send each block
    buffer = (char *) calloc(block_size, sizeof(char));
    for (i = 0; i < num_of_stripes; i++) {
        serverid_digits = snprintf(0,0,"%+d", server_id) - 1;
        i_digits = snprintf(0,0,"%+d", i) - 1;
        file_path_length = 5 + serverid_digits + 1 + ntohl(file_name_length) + 1 + i_digits;
        char *file_path = (char *) calloc(file_path_length, sizeof(char));
        snprintf(file_path, file_path_length, "data/%d_%s_%d", server_id, file_name, i);
        fp = fopen(file_path, "r");
        fread(buffer, block_size, 1, fp);
        if ((len = send(client_sd, buffer, block_size, 0)) < 0) {
            printf("Send Error: %s (Errno:%d)\n",strerror(errno),errno);
            exit(0);
        }
        fclose(fp);
    }
    free(buffer);
    free(file_name);
    free(file_path);
    fclose(mfp);
}

void list(int client_sd) {
    struct message_s reply;
    memset(&reply, 0, sizeof(reply));
    strncpy(reply.protocol, "myftp", 5);
    reply.type = 0xA2;
	struct dirent *entry;
    DIR *folder;
    folder = opendir("./data/metadata");
	char all_filename[512];
	int size = 1; // include null-terminated symbol
    while(entry = readdir(folder)) {
		char* filename;
		filename = entry->d_name;
		if(strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0) {
			size += strlen(entry->d_name) + 1;
			strcat(all_filename, filename);
			strcat(all_filename, "\n");
		}
    }
	all_filename[size] = 0;
	reply.length = htonl(sizeof(reply) + size);
	int len;
	if((len = send(client_sd, &reply, sizeof(reply), 0)) < 0){
		printf("Send Error: %s (Errno:%d)\n",strerror(errno),errno);
		exit(0);
	}
	if((len = send(client_sd, all_filename, size, 0)) < 0){
		printf("Send Error: %s (Errno:%d)\n",strerror(errno),errno);
		exit(0);
	}
}

void store_metadata(int file_size, char *file_name, int file_name_length){
    FILE *fp;
    char* file_path;
    file_path = (char *) calloc(14 + file_name_length, sizeof(char));
    strcpy(file_path, "data/metadata/");
    strcat(file_path, file_name);
    if ((fp = fopen(file_path, "w")) == NULL ) {
        printf("Error opening metadata file. Program terminated\n");
        exit(0);
    }
    fprintf(fp, "%d\n", file_size);
    fclose(fp);
    free(file_path);
}
