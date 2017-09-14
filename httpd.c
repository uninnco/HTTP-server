#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: uninncohttpd/0.1.0\r\n"

// 接受请求,并处理
void accept_request(int clientsockfd);
// 无法处理请求,回写400到client
void bad_request(int);
// 将文件内容发送到client
void cat(int client, FILE* fp);
// 不可以执行CGI程序,回写500到client
void cannot_execute(int);
// 输出错误信息
void error_die(const char *);
// 执行cgi程序
void execute_cgi(int, const char *, const char *, const char *);
// 从fd中读取一行,并将读取的'\r\n'和'\r'转换为'\n'
int get_line(int fd, char* buf, int bufsize);
// 返回200 OK给客户端
void headers(int clientsockfd, const char * filename);
// 返回404状态给客户端
void not_found(int);
// 处理客户端文件请求,发送404或200+文件内容到client
void serve_file(int client, const char* filename);
// 开启soket监听
int startup(u_short *);
// 返回501状态给客户端
void unimplemented(int);

/**********************************************************************/
/* A request has caused a call to accept() on the server port to
 * return.  Process the request appropriately.
 * Parameters: the socket connected to the client */
/**********************************************************************/
void accept_request(int client)
{
	char	buf[1024];
	int		numchars;
	char	method[255];	// 方法(请求类型)
	char	url[255];		// 请求的资源URL
	char	path[512];		// 请求资源的本地路径
	size_t i, j;
	struct stat st;
	int cgi = 0;		/* becomes true if server decides this is a CGI program */
						/* 为真时,表示服务器需调用一个CGI程序 */

	char *query_string = NULL;
	// 1. 从客户端连接请求数据包中读取一行
	numchars = get_line(client, buf, sizeof(buf));
	// 2. 从读取的数据中提取出请求类型
	i = 0; j = 0;
	while (!ISspace(buf[j]) && (i < sizeof(method) - 1))
	{
		method[i] = buf[j];
		i++; j++;
	}
	method[i] = '\0';
	// 3. 判断请求类型
	if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))
	{
		unimplemented(client);	// 不支持请求类型,向客户端返回501
		return;
	}

	if (strcasecmp(method, "POST") == 0){
		cgi = 1;	// 如果是POST请求,调用cgi程序
	}

	// 4. 提取请求的资源URL
	i = 0;
	while (ISspace(buf[j]) && (j < sizeof(buf))){
		j++;	// 定位下一个非空格位置
	}
	while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf)))
	{
		url[i] = buf[j];
		i++; j++;
	}
	url[i] = '\0';

	if (strcasecmp(method, "GET") == 0)
	{
		query_string = url;	// 为GET请求,查询语句为url
		// 如果查询语句中含义'?',查询语句为'?'字符后面部分
		while ((*query_string != '?') && (*query_string != '\0')){
			query_string++;
		}
		if (*query_string == '?')
		{
			cgi = 1;
			*query_string = '\0';	// 从'?'处截断,前半截为url
			query_string++;
		}
	}

	// 5. 生成本地路径
	sprintf(path, "htdocs%s", url);
	if (path[strlen(path) - 1] == '/'){
		strcat(path, "index.html");	//如果请求url是以'/'结尾,则指定为该目录下的index.html文件
	}
	// 6. 判断请求资源的状态
	if (stat(path, &st) == -1) {
		// 从client中读取,直到遇到两个换行(起始行startline和首部header之间间隔)
		while ((numchars > 0) && strcmp("\n", buf)){  /* read & discard headers */
			numchars = get_line(client, buf, sizeof(buf));
		}
		not_found(client);	// 501
	}
	else
	{
		// 请求的是个目录,指定为该目录下的index.html文件
		if ((st.st_mode & S_IFMT) == S_IFDIR){
			strcat(path, "/index.html");
		}
		// 请求的是一个可执行文件,作为CGI程序
		if ((st.st_mode & S_IXUSR) ||
			(st.st_mode & S_IXGRP) ||
			(st.st_mode & S_IXOTH)    ){
		  cgi = 1;
		}
		// 判断是执行一个CGI程序还是返回一个文件内容给客户端
		if (!cgi){
		  serve_file(client, path);	// 
		}
		else{
		  execute_cgi(client, path, method, query_string);
		}
	}

	close(client);
}

/**********************************************************************/
/* Inform the client that a request it has made has a problem.
 * Parameters: client socket */
/**********************************************************************/
void bad_request(int client)
{
	char buf[1024];
	// 向client回写400状态
	sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "<P>Your browser sent a bad request, ");
	send(client, buf, sizeof(buf), 0);
	sprintf(buf, "such as a POST without a Content-Length.\r\n");
	send(client, buf, sizeof(buf), 0);
}

/**********************************************************************/
/* Put the entire contents of a file out on a socket.  This function
 * is named after the UNIX "cat" command, because it might have been
 * easier just to do something like pipe, fork, and exec("cat").
 * Parameters: the client socket descriptor
 *             FILE pointer for the file to cat */
/**********************************************************************/
void cat(int client, FILE *resource)
{
	char buf[1024];
	// 从resource中读取一行
	fgets(buf, sizeof(buf), resource);
	// 将文件中的内容发送到客户端
	while (!feof(resource))
	{
		send(client, buf, strlen(buf), 0);
		fgets(buf, sizeof(buf), resource);
	}
}

/**********************************************************************/
/* Inform the client that a CGI script could not be executed.
 * Parameter: the client socket descriptor. */
/**********************************************************************/
void cannot_execute(int client)
{
	char buf[1024];
	// 向客户端回写500状态,不可以执行CGI程序
	sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Print out an error message with perror() (for system errors; based
 * on value of errno, which indicates system call errors) and exit the
 * program indicating an error. */
/**********************************************************************/
void error_die(const char *sc)
{
	perror(sc);	// 输出错误信息
	exit(1);	// 退出程序
}

/********************************************************************//*
 * @brief	执行一个CGI脚本.可能需要设置适当的环境变量.
 * @prama	client	客户端socket文件描述符
 * @prama	path	CGI 脚本路径
 * @prama	method	请求类型
 * @prama	query_string	查询语句
 **********************************************************************/
void execute_cgi(int client, const char *path,
			const char *method, const char *query_string)
{
	char	buf[1024];
	int		cgi_output[2];	// CGI程序输出管道
	int		cgi_input[2];	// CGI程序输入管道

	pid_t	pid;
	int		status;
	int		i;
	char	c;
	int		numchars = 1;
	int		content_length = -1;

	buf[0] = 'A'; buf[1] = '\0';

	if (strcasecmp(method, "GET") == 0){
		// 从client中读取,直到遇到两个换行
		while ((numchars > 0) && strcmp("\n", buf)){  /* read & discard headers */
			numchars = get_line(client, buf, sizeof(buf));
		}
	}
	else    /* POST */
	{
		// 获取请求主体的长度
		numchars = get_line(client, buf, sizeof(buf));
		while ((numchars > 0) && strcmp("\n", buf))
		{
			buf[15] = '\0';
			if (strcasecmp(buf, "Content-Length:") == 0){
			  content_length = atoi(&(buf[16]));
			}
			numchars = get_line(client, buf, sizeof(buf));
		}
		if (content_length == -1) {
			bad_request(client);
			return;
		}
	}

	// 向client回写200 OK
	sprintf(buf, "HTTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);

	// 创建两个匿名管道
	if (pipe(cgi_output) < 0) {
		cannot_execute(client);
		return;
	}
	if (pipe(cgi_input) < 0) {
		cannot_execute(client);
		return;
	}

	// 创建子进程,去执行CGI程序
	if ( (pid = fork()) < 0 ) {
		cannot_execute(client);
		return;
	}
	if (pid == 0)  /* child: CGI script */
	{
		char meth_env[255];
		char query_env[255];
		char length_env[255];

		dup2(cgi_output[1], 1);	//复制cgi_output[1](读端)到子进程的标准输出
		dup2(cgi_input[0], 0);	//复制cgi_input[0](写端)到子进程的标准输入
		close(cgi_output[0]);	//关闭多余文件描述符
		close(cgi_input[1]);

		sprintf(meth_env, "REQUEST_METHOD=%s", method);
		putenv(meth_env);	// 添加一个环境变量

		if (strcasecmp(method, "GET") == 0) {
			sprintf(query_env, "QUERY_STRING=%s", query_string);
			putenv(query_env);
		}
		else {   /* POST */
			sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
			putenv(length_env);
		}
		// 执行CGI程序
		execl(path, path, NULL);
		exit(0);	// 子进程退出
	}
	else 
	{    /* parent */
		close(cgi_output[1]);	// 关闭cgi_output读端
		close(cgi_input[0]);	// 关闭cgi_input写端

		if (strcasecmp(method, "POST") == 0){
			// 请求类型为POST的时候,将POST数据包的主体entity-body部分
			// 通过cgi_input[1](写端)写入到CGI的标准输入
			for (i = 0; i < content_length; i++) {
				recv(client, &c, 1, 0);
				write(cgi_input[1], &c, 1);
			}
		}
		// 读取CGI的标准输出,发送到客户端
		while (read(cgi_output[0], &c, 1) > 0){
		  send(client, &c, 1, 0);
		}
		// 关闭多余文件描述符
		close(cgi_output[0]);
		close(cgi_input[1]);
		// 等待子进程结束
		waitpid(pid, &status, 0);
	}
}

/**********************************************************************/
/* Get a line from a socket, whether the line ends in a newline,
 * carriage return, or a CRLF combination.  Terminates the string read
 * with a null character.  If no newline indicator is found before the
 * end of the buffer, the string is terminated with a null.  If any of
 * the above three line terminators is read, the last character of the
 * string will be a linefeed and the string will be terminated with a
 * null character.
 * Parameters: the socket descriptor
 *             the buffer to save the data in
 *             the size of the buffer
 * Returns: the number of bytes stored (excluding null) */
/**********************************************************************/
int get_line(int sock, char *buf, int size)
{
	int		i = 0;
	char	c = '\0';
	int		n;

	while ((i < size - 1) && (c != '\n'))
	{
		// 从sock中读取一个字节
		n = recv(sock, &c, 1, 0);
		/* DEBUG printf("%02X\n", c); */
		if (n > 0)
		{
			// 将 \r\n 或 \r 转换为'\n'
			if (c == '\r')
			{
				// 读到了'\r'就再预读一个字节
				n = recv(sock, &c, 1, MSG_PEEK);
				/* DEBUG printf("%02X\n", c); */
				// 如果读取到的是'\n',就读取,否则c='\n'
				if ((n > 0) && (c == '\n'))
				  recv(sock, &c, 1, 0);
				else
				  c = '\n';
			}
			// 读取数据放入buf
			buf[i] = c;
			i++;
		}
		else{
		  c = '\n';
		}
	}
	buf[i] = '\0';
	// 返回写入buf的字节数
	return(i);
}

/**********************************************************************/
/* Return the informational HTTP headers about a file. */
/* Parameters: the socket to print the headers on
 *             the name of the file */
/**********************************************************************/
void headers(int client, const char *filename)
{
	char buf[1024];
	(void)filename;  /* could use filename to determine file type */
	
	// 向客户端回写200应答
	strcpy(buf, "HTTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);
	strcpy(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	strcpy(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Give a client a 404 not found status message. */
/**********************************************************************/
void not_found(int client)
{
	char buf[1024];
	// 向client回写404状态
	sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "your request because the resource specified\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "is unavailable or nonexistent.\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/
/* Send a regular file to the client.  Use headers, and report
 * errors to client if they occur.
 * Parameters: a pointer to a file structure produced from the socket
 *              file descriptor
 *             the name of the file to serve */
/**********************************************************************/
void serve_file(int client, const char *filename)
{
	FILE *resource = NULL;
	int numchars = 1;
	char buf[1024];

	buf[0] = 'A'; buf[1] = '\0';
	// 从client中读取,直到遇到两个换行(起始行start line和首部header的间隔)
	while ((numchars > 0) && strcmp("\n", buf)){  /* read & discard headers */
		numchars = get_line(client, buf, sizeof(buf));
	}
	// 
	resource = fopen(filename, "r");
	if (resource == NULL)
		not_found(client);	// 404错误
	else
	{
		headers(client, filename);	// 向客户端回写200 OK
		cat(client, resource);		// 将resource指向文件中的内容写入client
	}
	fclose(resource);
}

/**********************************************************************/
/* This function starts the process of listening for web connections
 * on a specified port.  If the port is 0, then dynamically allocate a
 * port and modify the original port variable to reflect the actual
 * port.
 * Parameters: pointer to variable containing the port to connect on
 * Returns: the socket */
/**********************************************************************/
int startup(u_short *port)
{
	int httpd = 0;
	struct sockaddr_in name;
	// 创建一个socket
	httpd = socket(PF_INET, SOCK_STREAM, 0);
	if (httpd == -1){
		error_die("socket");
	}
	// 填写绑定地址
	memset(&name, 0, sizeof(name));
	name.sin_family = AF_INET;
	name.sin_port = htons(*port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);
	// 绑定socket到指定地址
	if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0){
		error_die("bind");
	}
	// 如果采用随机分配端口,获取它
	if (*port == 0)  /* if dynamically allocating a port */
	{
		int namelen = sizeof(name);
		if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
		  error_die("getsockname");
		*port = ntohs(name.sin_port);
	}
	// 监听,等待连接
	if (listen(httpd, 5) < 0)
	  error_die("listen");
	// 返回正在监听的文件描述符
	return(httpd);
}

/**********************************************************************/
/* Inform the client that the requested web method has not been
 * implemented.
 * Parameter: the client socket */
/**********************************************************************/
void unimplemented(int client)
{
	char buf[1024];
	// 向client回写501状态
	sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</TITLE></HEAD>\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}

/**********************************************************************/

int main(void)
{
	int server_sock = -1;
	u_short port	= 0;	// 使用随机端口
	int client_sock = -1;
	struct sockaddr_in client_name;
	int client_name_len = sizeof(client_name);
	pthread_t newthread;
	
	// 启动服务,监听,等待连接
	server_sock = startup(&port);
	printf("httpd running on port %d\n", port);

	while (1)
	{
		// 接受请求
		client_sock = accept(server_sock,
					(struct sockaddr *)&client_name,
					&client_name_len);
		if (client_sock == -1)
			error_die("accept");
		// 创建一个线程来处理请求
		/* accept_request(client_sock); */
		if (pthread_create(&newthread , NULL, accept_request, client_sock) != 0)
			perror("pthread_create");
	}
	// 关闭
	close(server_sock);

	return(0);
}
