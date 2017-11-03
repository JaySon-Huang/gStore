/*=============================================================================
# Filename: ghttp.cpp
# Author: Bookug Lobert 
# Mail: zengli-bookug@pku.edu.cn
# Last Modified: 2017-06-15 15:09
# Description: created by lvxin, improved by lijing
=============================================================================*/

//SPARQL Endpoint:  log files in logs/endpoint/
//operation.log: not used
//query.log: query string, result num, and time of answering

//TODO: to add db_name to all URLs, and change the index.js using /show to get name, save and set
//TODO: use gzip for network transfer, it is ok to download a gzip file instead of the original one
//DEBUG: if port has already been occupied, the server will be restarted endlessly, here we should stop

#include "../Server/server_http.hpp"
#include "../Server/client_http.hpp"
//db
#include "../Database/Database.h"
#include "../Util/Util.h"

using namespace std;
//Added for the json-example:
using namespace boost::property_tree;

typedef SimpleWeb::Server<SimpleWeb::HTTP> HttpServer;
typedef SimpleWeb::Client<SimpleWeb::HTTP> HttpClient;

//int initialize();
int initialize(int argc, char *argv[]);
//Added for the default_resource example
void default_resource_send(const HttpServer &server, const shared_ptr<HttpServer::Response> &response,
        const shared_ptr<ifstream> &ifs);
void download_result(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request, string download, string filepath);

pthread_t start_thread(void *(*_function)(void*));
bool stop_thread(pthread_t _thread);
void* func_timer(void* _args);
void* func_scheduler(void* _args);
void thread_sigterm_handler(int _signal_num);

//=============================================================================
bool build_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);

bool load_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);

bool unload_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);

void query_handler(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);

bool monitor_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);

bool checkpoint_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);

bool show_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);

bool delete_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);

bool download_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);

bool default_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request);
//=============================================================================

//TODO: use locak to protect logs when running in multithreading environment
FILE* query_logfp = NULL;
string queryLog = "logs/endpoint/query.log";
mutex query_log_lock;
pthread_rwlock_t database_load_lock;

Database *current_database = NULL;
int connection_num = 0;

long next_backup = 0;
pthread_t scheduler = 0;

int query_num = 0;
//WARN: if the response or request is freed by main thread
void test_thread(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	int num = query_num;
	cout<<"test thread: "<<num<<endl;
	sleep(10);
	string error = "test thread here: query ";
	//*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error<<query_num;
	*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length()+4;
	*response << "\r\nContent-Type: text/plain";
	*response << "\r\nCache-Control: no-cache" << "\r\nPragma: no-cache" << "\r\nExpires: 0";
	*response << "\r\n\r\n" << error<<num;
	cout<<"thread ends: "<<num<<endl;
}

//DEBUG+TODO: why the result transfered to client has no \n \t??
//using json is a good way to avoid this problem

//BETTER: How about change HttpConnector into a console?
//TODO: we need a route
//JSON parser: http://www.tuicool.com/articles/yUJb6f     
//(or use boost spirit to generate parser when compiling)
//
//NOTICE: no need to close connection here due to the usage of shared_ptr
//http://www.tuicool.com/articles/3Ub2y2
//
//TODO: the URL format is terrible, i.e. 127.0.0.1:9000/build/lubm/data/LUBM_10.n3
//we should define some keys like operation, database, dataset, query, path ...
//127.0.0.1:9000?operation=build&database=lubm&dataset=data/LUBM_10.n3
//
//TODO: control the authority, check it if requesting for build/load/unload
//for sparql endpoint, just load database when starting, and comment out all functions except for query()

//REFERENCE: C++ URL encoder and decoder
//http://blog.csdn.net/nanjunxiao/article/details/9974593
string UrlDecode(string& SRC)
{
	string ret;
	char ch;
	int ii;
	for(size_t i = 0; i < SRC.length(); ++i)
	{
		if(int(SRC[i]) == 37)
		{
			sscanf(SRC.substr(i+1,2).c_str(), "%x", &ii);
			ch = static_cast<char>(ii);
			ret += ch;
			i = i + 2;
		}
		else if(SRC[i] == '+')
		{
			ret += ' ';
		}
		else
		{
			ret += SRC[i];
		}
	}
	return (ret);
}

int main(int argc, char *argv[])
{
	Util util;
	srand(time(NULL));

	//Notice that current_database is assigned in the child process, not in the father process
	//when used as endpoint, or when the database is indicated in command line, we can assign 
	//current_database in father process(but this is resource consuming)
	//TODO+DEBUG: when will the restarting error?
	while (true) {
		//NOTICE: here we use 2 processes, father process is used for monitor and control(like, restart)
		//Child process is used to deal with web requests, can also has many threads
		pid_t fpid = fork();

		if (fpid == 0) {
			//int ret = initialize();
			int ret = initialize(argc, argv);
			//TODO: which to use here!
			exit(ret);
			//return ret;
		}

		else if (fpid > 0) {
			int status;
			waitpid(fpid, &status, 0);
			if (WIFEXITED(status)) {
				//exit(0);
				return 0;
			}
			else
			{
				cerr << "Server stopped abnormally, restarting server..." << endl;
				//break;
			}
		}

		else {
			cerr << "Failed to start server: deamon fork failure." << endl;
			return -1;
		}

	}

	return 0;
}

bool isNum(char *str)
{
	for(int i = 0; i < strlen(str); i++)
	{
		int tmp = (int)(str[i]);
		if(tmp < 48 || tmp > 57)
			return false;
	}
	return true;
}

int initialize(int argc, char *argv[]) 
{
    cout << "enter initialize." << endl;
	//Server restarts to use the original database
	//current_database = NULL;

	HttpServer server;
	string db_name;
	if(argc == 1)
	{
		server.config.port = 9000;
		db_name = "";
	}
	else if(argc == 2)
	{
		if(isNum(argv[1]))
		{
			server.config.port = atoi(argv[1]);
			db_name = "";
		}
		else
		{
			server.config.port = 9000;
			db_name = argv[1];
		}
	}
	else
	{
		if(isNum(argv[1]))
		{
			server.config.port = atoi(argv[1]);
			db_name = argv[2];
		}
		else if(isNum(argv[2]))
		{
			server.config.port = atoi(argv[2]);
			db_name = argv[1];
		}
		else
		{
			cout << "wrong format of parameters, please input the server port and the database." << endl;
			return -1;
		}
	}
	cout << "server port: " << server.config.port << " database name: " << db_name << endl;
	//USAGE: then user can use http://localhost:port/ to visit the server or coding with RESTful API
    //HTTP-server at port 9000 using 1 thread
    //Unless you do more heavy non-threaded processing in the resources,
    //1 thread is usually faster than several threads

    //GET-example for the path /load/[db_name], responds with the matched string in path
    //For instance a request GET /load/db123 will receive: db123
    //server.resource["^/load/(.*)$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {

	string database = db_name;
	if(current_database == NULL && database != "")
	{
		if(database.length() > 3 && database.substr(database.length()-3, 3) == ".db")
		{
			cout << "Your db name to be built should not end with \".db\"." << endl;
			return -1;
		}
	
		cout << database << endl;
		current_database = new Database(database);
		bool flag = current_database->load();
		if (!flag)
		{
			cout << "Failed to load the database."<<endl;
			delete current_database;
			current_database = NULL;
			return -1;
		}
		//string success = db_name;
		cout << "Database loaded successfully."<<endl;
	}

	//open the query log
	query_logfp = fopen(queryLog.c_str(), "a");
	if(query_logfp == NULL)
	{
		cerr << "open query log error"<<endl;
		return -1;
	}

	//time_t cur_time = time(NULL);
	//long time_backup = Util::read_backup_time();
	//long next_backup = cur_time - (cur_time - time_backup) % Util::gserver_backup_interval + Util::gserver_backup_interval;
	//NOTICE: no need to backup for endpoint
//TODO: we give up the backup function here
#ifndef ONLY_READ
	//scheduler = start_thread(func_scheduler);
#endif


	pthread_rwlock_init(&database_load_lock, NULL);

#ifndef SPARQL_ENDPOINT
	//GET-example for the path /?operation=build&db_name=[db_name]&ds_path=[ds_path], responds with the matched string in path
	//i.e. database name and dataset path
	server.resource["^/%3[F|f]operation%3[D|d]build%26db_name%3[D|d](.*)%26ds_path%3[D|d](.*)$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		build_handler(server, response, request);
    };

	//GET-example for the path /?operation=load&db_name=[db_name], responds with the matched string in path
    server.resource["^/%3[F|f]operation%3[D|d]load%26db_name%3[D|d](.*)$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		load_handler(server, response, request);
	};

    //GET-example for the path /?operation=unload&db_name=[db_name], responds with the matched string in path
    server.resource["^/%3[F|f]operation%3[D|d]unload%26db_name%3[D|d](.*)$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		unload_handler(server, response, request);
    }; 

	//GET-example for the path /?operation=build&db_name=[db_name]&ds_path=[ds_path], responds with the matched string in path
	//i.e. database name and dataset path
	server.resource["^/?operation=build&db_name=(.*)&ds_path=(.*)$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		build_handler(server, response, request);
    };

	//GET-example for the path /?operation=load&db_name=[db_name], responds with the matched string in path
    server.resource["^/?operation=load&db_name=(.*)$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		load_handler(server, response, request);
	};

    //GET-example for the path /?operation=unload&db_name=[db_name], responds with the matched string in path
    server.resource["^/?operation=unload&db_name=(.*)$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		unload_handler(server, response, request);
    };
#endif
	
	
	//GET-example for the path /?operation=query&format=[format]&sparql=[sparql], responds with the matched string in path
	 server.resource["^/%3[F|f]operation%3[D|d]query%26format%3[D|d](.*)%26sparql%3[D|d](.*)$"]["GET"] = [&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request)
	 {
		query_handler(response, request);
	 };


    //NOTICE:this may not be visited by browser directly if the browser does not do URL encode automatically!
	//In programming language, do URL encode first and then call server, then all is ok
	server.resource["^/?operation=monitor$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
	 //server.resource["^/monitor$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {
		monitor_handler(server, response, request);
    };
   
    // server.resource["^/json$"]["POST"]=[](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {
    //     try {
    //         ptree pt;
    //         read_json(request->content, pt);

    //         string sparql=pt.get<string>("queryContent");

    //         *response << "HTTP/1.1 200 OK\r\n"
    //             << "Content-Type: application/json\r\n"
    //             << "Content-Length: " << name.length() << "\r\n\r\n"
    //             << name;
    //     }
    //     catch(exception& e) {
    //         *response << "HTTP/1.1 400 Bad Request\r\nContent-Length: " << strlen(e.what()) << "\r\n\r\n" << e.what();
    //     }
    // };

	//TODO: use db_name if multiple databases
    server.resource["^/%3[F|f]operation%3[D|d]checkpoint$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		checkpoint_handler(server, response, request);
    };

	//TODO: add user name as parameter, current using or all databases availiable
    server.resource["^/%3[F|f]operation%3[D|d]show$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		show_handler(server, response, request);
    };

	//USAGE: in endpoint, if user choose to display via html, but not display all because the result's num is too large.
	//Then, user can choose to download all results in TXT format, and this URL is used for download in this case
	//WARN: this URL rule should not be published!
	//
	//NOTICE: we need to remove temp result files after download once, otherwise the disk of server will be run out easily.
	//
	//Safety: user can attack by using this URL and point a specific filePath to be removed, which is not the download one!
	//We do not have the http session ID here and can not provide best safety, but we can set the filePath complicated.
	//(then attacker is hard to guess, and the directory is restricted)
	//BETTER+TODO: associate the thread/session ID with download fileName, otherwise error may come in parallism
	//
	//GET-example for the path /?operation=delete&filepath=[filepath], responds with the matched string in path
	server.resource["^/%3[F|f]operation%3[D|d]delete%26filepath%3[D|d](.*)$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		delete_handler(server, response, request);
    };
	//GET-example for the path /?operation=delete&filepath=[filepath], responds with the matched string in path
	server.resource["^/%3[F|f]operation%3[D|d]download%26filepath%3[D|d](.*)$"]["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		download_handler(server, response, request);
    };
    //Default GET-example. If no other matches, this anonymous function will be called.
    //Will respond with content in the web/-directory, and its subdirectories.
    //Default file: index.html
    //Can for instance be used to retrieve an HTML 5 client that uses REST-resources on this server
    server.default_resource["GET"]=[&server](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) 
	{
		default_handler(server, response, request);
    };

    thread server_thread([&server](){
            //Start server
            server.start();
			cout<<"check: server started"<<endl;
            });

    //Wait for server to start so that the client can connect
    this_thread::sleep_for(chrono::seconds(1));

    // //Client examples
    // HttpClient client("localhost:9000");
    // auto r1=client.request("GET", "/match/123");
    // cout << r1->content.rdbuf() << endl;

    // string json_string="{\"firstName\": \"John\",\"lastName\": \"Smith\",\"age\": 25}";
    // auto r2=client.request("POST", "/string", json_string);
    // cout << r2->content.rdbuf() << endl;

    // auto r3=client.request("POST", "/json", json_string);
    // cout << r3->content.rdbuf() << endl;

    server_thread.join();
	cout<<"check: server stoped"<<endl;
	pthread_rwlock_destroy(&database_load_lock);

    return 0;
}

//QUERY: can server.send() in default_resource_send run in parallism?

//TCP is slower than UDP, but more safely, and can support large data transfer well
//http://blog.csdn.net/javaniuniu/article/details/51419348
void default_resource_send(const HttpServer &server, const shared_ptr<HttpServer::Response> &response,
        const shared_ptr<ifstream> &ifs) {
    //read and send 128 KB at a time
    static vector<char> buffer(131072); // Safe when server is running on one thread
    streamsize read_length;
	//cout<<"int 0"<<endl;
    if((read_length=ifs->read(&buffer[0], buffer.size()).gcount())>0) {
        response->write(&buffer[0], read_length);
	//cout<<"int 1"<<endl;
        if(read_length==static_cast<streamsize>(buffer.size())) {
	//cout<<"int 2"<<endl;
            server.send(response, [&server, response, ifs](const boost::system::error_code &ec) {
	//cout<<"int 3"<<endl;
                    if(!ec)
                    default_resource_send(server, response, ifs);
                    else
                    cerr << "Connection interrupted" << endl;
                    });
        }
    }
}

pthread_t start_thread(void *(*_function)(void*)) {
	pthread_t thread;
	if (pthread_create(&thread, NULL, _function, NULL) == 0) {
		return thread;
	}
	return 0;
}

bool stop_thread(pthread_t _thread) {
	return pthread_kill(_thread, SIGTERM) == 0;
}

//DEBUG+TODO: the whole process exits and need to reload the database
//(too slow: quit and restart)
void* func_timer(void* _args) {
	signal(SIGTERM, thread_sigterm_handler);
	sleep(Util::gserver_query_timeout);
	cerr << "Query out of time." << endl;
	//TODO: not use SIGTERM and abort() here, which will cause process to quit abnormally
	//here shoudl just end the timer thread 
	abort();
}

void* func_scheduler(void* _args) {
	signal(SIGTERM, thread_sigterm_handler);
	while (true) {
		time_t cur_time = time(NULL);
		while (cur_time >= next_backup) {
			next_backup += Util::gserver_backup_interval;
		}
		sleep(next_backup - cur_time);
		if (!current_database->backup()) {
			return NULL;
		}
	}
}

void thread_sigterm_handler(int _signal_num) {
	pthread_exit(0);
}

//NOTICE: what if several requests to delete the same file? we assume all are valid and no attackers
void delete_thread(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<< "HTTP: this is delete" << endl;

	string filepath = request->path_match[1];
	filepath = UrlDecode(filepath);
	try {
		//set the home directory of the web server
		//NOTICE: we use .tmp/web instead of . to avoid attack: delete other files rather than the download one
		auto root_path=boost::filesystem::canonical(".tmp/web");
		auto path=boost::filesystem::canonical(root_path/filepath);
		cout << "path: " << path << endl;
		//Check if path is within root_path
		if(distance(root_path.begin(), root_path.end())>distance(path.begin(), path.end()) ||
				!equal(root_path.begin(), root_path.end(), path.begin()))
			throw invalid_argument("path must be within root path.");
		/*
		if(download == "true")
		{
			std::string cache_control, etag;

			// Uncomment the following line to enable Cache-Control
			// cache_control="Cache-Control: max-age=86400\r\n";
			
			//return file in path
			auto ifs=make_shared<ifstream>();
			ifs->open(path.string(), ifstream::in | ios::binary | ios::ate);

			if(*ifs) {
		//cout<<"open file!!!"<<endl;
				auto length=ifs->tellg();
				ifs->seekg(0, ios::beg);

				*response << "HTTP/1.1 200 OK\r\n" << cache_control << etag << "Content-Length: " << length << "\r\n\r\n";
				default_resource_send(server, response, ifs);
				//DEBUG: in fact this make no sense, result will be received by clients even if server not wait for the network transfer!  (but very slow if results are large)
				//sleep(10);  //seconds
				//cout<<"sleep ends"<<endl;
				//DEBUG: this will close the stream, and the info received by client may be incomplete
				//ifs->close();
			}
			else
				throw invalid_argument("could not read file.");
		}
		*/
		if((boost::filesystem::exists(path) && boost::filesystem::is_regular_file(path)))
		{
			//delete file in delpath.
			char name[60];
			strcpy(name, path.c_str());
			cout << "name: " << name << endl;
			if(remove(name) == -1)
			{
				cout << "could not delete file." << endl;
				perror("remove");
			}
			else
			{
				cout << "file delete." << endl;
				//Notice: if file deleted successfully, service need to response to the browser, if not the browser will not execute the call-back function.
				string success = "file delete successfully.";
				*response << "HTTP/1.1 200 OK\r\nContent-Length: " << success.length() << "\r\n\r\n" << success;
			}
		}	
	}
	catch(const exception &e) {
		//cout<<"can not open file!!!"<<endl;
		string content="Could not open path "+request->path+": "+e.what();
		*response << "HTTP/1.1 400 Bad Request\r\nContent-Length: " << content.length() << "\r\n\r\n" << content;
	}
}

void download_thread(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<< "HTTP: this is download" << endl;

	string filepath = request->path_match[1];
	filepath = UrlDecode(filepath);
	try {
		//set the home directory of the web server
		//NOTICE: we use .tmp/web instead of . to avoid attack: delete other files rather than the download one
		cout << "filepath: " << filepath << endl;
		auto root_path=boost::filesystem::canonical(".tmp/web");
		cout << "root_path: " << root_path << endl;
		auto path=boost::filesystem::canonical(root_path/filepath);
		cout << "path: " << path << endl;
		//Check if path is within root_path
		if(distance(root_path.begin(), root_path.end())>distance(path.begin(), path.end()) ||
				!equal(root_path.begin(), root_path.end(), path.begin()))
			throw invalid_argument("path must be within root path.");

		std::string cache_control, etag;

		// Uncomment the following line to enable Cache-Control
		// cache_control="Cache-Control: max-age=86400\r\n";
			
		//return file in path
		auto ifs=make_shared<ifstream>();
		ifs->open(path.string(), ifstream::in | ios::binary | ios::ate);

		if(*ifs) {
			//cout<<"open file!!!"<<endl;
			auto length=ifs->tellg();
			ifs->seekg(0, ios::beg);
			//!Notice: remember to set the Content-Disposition and the Content-type to let the browser to download.
			*response << "HTTP/1.1 200 OK\r\n" << "Content-Length: " << length << "\r\n";
			*response << "Content-Disposition: attachment; filename=sparql.txt" << "\r\n";
			*response << "Content-Type: application/octet-stream" << "\r\n\r\n";
			default_resource_send(server, response, ifs);
		}
		else
			throw invalid_argument("could not read file.");
		/*
		if((boost::filesystem::exists(path) && boost::filesystem::is_regular_file(path)))
		{
			//delete file in delpath.
			char name[60];
			strcpy(name, path.c_str());
			cout << "name: " << name << endl;
			if(remove(name) == -1)
			{
				cout << "could not delete file." << endl;
				perror("remove");
			}
			else
				cout << "file delete." << endl;
		}
		*/
	}
	catch(const exception &e) {
		//cout<<"can not open file!!!"<<endl;
		string content="Could not open path "+request->path+": "+e.what();
		*response << "HTTP/1.1 400 Bad Request\r\nContent-Length: " << content.length() << "\r\n\r\n" << content;
	}
}

void build_thread(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<<"HTTP: this is build"<<endl;

	string db_name=request->path_match[1];
	string db_path=request->path_match[2];
	db_name = UrlDecode(db_name);
	db_path = UrlDecode(db_path);
	if(db_name=="" || db_path=="")
	{
		string error = "Exactly 2 arguments required!";
		// error = db_name + " " + db_path;
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	string database = db_name;
	if(database.length() > 3 && database.substr(database.length()-3, 3) == ".db")
	{
		string error = "Your db name to be built should not end with \".db\".";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	//database += ".db";
	string dataset = db_path;

	if(pthread_rwlock_tryrdlock(&database_load_lock) != 0)
	{
		string error = "Unable to build due to the loss of lock!";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	if(current_database != NULL)
	{
		string error = "Please unload your database first.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}

	cout << "Import dataset to build database..." << endl;
	cout << "DB_store: " << database << "\tRDF_data: " << dataset << endl;
	int len = database.length();

	pthread_rwlock_unlock(&database_load_lock);
	if(pthread_rwlock_trywrlock(&database_load_lock) != 0)
	{
		string error = "Unable to build due to the loss of lock!";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	current_database = new Database(database);
	bool flag = current_database->build(dataset);
	delete current_database;
	current_database = NULL;

	if(!flag)
	{
		string error = "Import RDF file to database failed.";
		string cmd = "rm -r " + database;
		system(cmd.c_str());
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}

	// string success = db_name + " " + db_path;
	string success = "Import RDF file to database done.";
	*response << "HTTP/1.1 200 OK\r\nContent-Length: " << success.length() << "\r\n\r\n" << success;
	pthread_rwlock_unlock(&database_load_lock);
}

bool build_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	thread t(&build_thread, response, request);
	t.detach();
	return true;
}

void load_thread(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<<"HTTP: this is load"<<endl;

	string db_name = request->path_match[1];
	db_name = UrlDecode(db_name);

   //	string db_name = argv[1];
	if(db_name=="")
	{
		string error = "Exactly 1 argument is required!";
		*response << "HTTP/1.1 200 ok\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	string database = db_name;
	if(database.length() > 3 && database.substr(database.length()-3, 3) == ".db")
	{
		//cout << "Your db name to be built should not end with \".db\"." << endl;
	   string error = "Your db name to be built should not end with \".db\".";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	if(pthread_rwlock_tryrdlock(&database_load_lock) != 0)
	{
	    string error = "Unable to load due to loss of lock";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	//database += ".db";
	if(current_database != NULL)
	{
		//cout << "Please unload your current database first." <<endl;
		string error = "Please unload your current database first.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}
	cout << database << endl;

	pthread_rwlock_unlock(&database_load_lock);
	if(pthread_rwlock_trywrlock(&database_load_lock) != 0)
	{
	    string error = "Unable to load due to loss of lock";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	current_database = new Database(database);
	bool flag = current_database->load();
	if (!flag)
	{
		string error = "Failed to load the database.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		delete current_database;
		current_database = NULL;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}

	//TODO+DEBUG: if we use the scheduler here, then each load will bring an asleep thread!
	//time_t cur_time = time(NULL);
	//long time_backup = Util::read_backup_time();
	//long next_backup = cur_time - (cur_time - time_backup) % Util::gserver_backup_interval + Util::gserver_backup_interval;
	//scheduler = start_thread(func_scheduler);
	
//string success = db_name;
//cout << "Database loaded successfully." << endl;
	string success = "Database loaded successfully.";
	*response << "HTTP/1.1 200 OK\r\nContent-Length: " << success.length() << "\r\n\r\n" << success;
	pthread_rwlock_unlock(&database_load_lock);
}

bool load_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	thread t(&load_thread, response, request);
	t.detach();
	return true;
}

void unload_thread(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<<"HTTP: this is unload"<<endl;

	string db_name = request->path_match[1];
	db_name = UrlDecode(db_name);

   //	string db_name = argv[1];
	if(db_name=="")
	{
		string error = "Exactly 1 argument is required!";
		*response << "HTTP/1.1 200 ok\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	string database = db_name;
	if(database.length() > 3 && database.substr(database.length()-3, 3) == ".db")
	{
		//cout << "Your db name to be built should not end with \".db\"." << endl;
	   string error = "Your db name to be built should not end with \".db\".";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	if(pthread_rwlock_tryrdlock(&database_load_lock) != 0)
	{
	    string error = "Unable to unload due to loss of lock";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	//database += ".db";
	if(current_database == NULL)
	{
		string error = "No database used now.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}
	cout << database << endl;

	if(current_database->getName() != database)
	{
		string error = "Database Name not matched.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}

	pthread_rwlock_unlock(&database_load_lock);
	if(pthread_rwlock_trywrlock(&database_load_lock) != 0)
	{
	    string error = "Unable to unload due to loss of lock";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	delete current_database;
	current_database = NULL;
	string success = "Database unloaded.";
	*response << "HTTP/1.1 200 OK\r\nContent-Length: " << success.length() << "\r\n\r\n" << success;
	pthread_rwlock_unlock(&database_load_lock);
}

bool unload_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	thread t(&unload_thread, response, request);
	t.detach();
	return true;
}

void writeLog(FILE* fp, string _info)
{
	//Another way to locka many: lock(lk1, lk2...)
	query_log_lock.lock();
	fprintf(fp, "%s", _info.c_str());
	Util::Csync(fp);
	query_log_lock.unlock();
}

void query_thread(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	//Notice: update the log in mutithreading environment
	//1. add therad ID to each line
	//2. combine information in a therad then write
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<<"HTTP: this is query"<<endl;
	//sleep(5);

	string format = request->path_match[1];
	//string format = "html";
	string db_query=request->path_match[2];
	format = UrlDecode(format);
	db_query = UrlDecode(db_query);
	cout<<log_prefix<<"check: "<<db_query<<endl;
	string str = db_query;

	if(pthread_rwlock_tryrdlock(&database_load_lock) != 0)
	{
		string error = "Unable to query due to the loss of lock!";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	if(current_database == NULL)
	{
		string error = "No database in use!";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}

	string sparql;
	sparql = db_query;
	if (sparql.empty()) {
		cerr <<log_prefix<< "Empty SPARQL." << endl;
		string error = "Empty SPARQL.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}
	FILE* output = NULL;

	//just for DEBUG
	//sleep(3);
	//*response << "HTTP/1.1 200 OK\r\nContent-Length: " << sparql.length() << "\r\n\r\n" << sparql;
	//cout<<log_prefix<<"ends here"<<endl;
	//return; 

	//TODO+DEBUG: manage the timer, not causing the db to restart
	//pthread_t timer = start_thread(func_timer);
	//if (timer == 0) {
		//cerr <<log_prefix<< "Failed to start timer." << endl;
	//}

	ResultSet rs;
	int query_time = Util::get_cur_time();
	//TODO: extract the parser and do sequentially first, search or update
	int ret_val = current_database->query(sparql, rs, output);
	bool ret = (ret_val == -100);

	query_time = Util::get_cur_time() - query_time;
	//if (timer != 0 && !stop_thread(timer)) 
	//{
		//cerr <<log_prefix<< "Failed to stop timer." << endl;
	//}

	ostringstream stream;
	stream << rs.ansNum;
	string ansNum_s = stream.str();
	cout <<log_prefix<< "ansNum_s: " << ansNum_s << endl;
	string filename = "thread_" + thread_id + "_" + Util::getTimeName() + "_query";
	string localname = ".tmp/web/" + filename;

	if(ret)
	{
		cout <<log_prefix<< "query returned successfully." << endl;
		
		//record each query operation, including the sparql and the answer number
		string log_info = Util::get_date_time() + "\n" + sparql + "\n\nanswer num: " + Util::int2string(rs.ansNum)+"\nquery time: "+Util::int2string(query_time) +" ms\n-----------------------------------------------------------\n";
		writeLog(query_logfp, log_info);

		ofstream outfile;
		string ans = "";
		string success = "";
	//TODO: if result is stored in Stream instead of memory?  (if out of memory to use to_str)
	//BETTER: divide and transfer, in multiple times, getNext()
		if(format == "json")
		{
		//	cout << "query success, transfer to json." << endl;
			success = rs.to_JSON();
		}
		else
		{
		//	cout << "query success, transfer to str." << endl;
			success = rs.to_str();
		}
		if(format == "html")
		{
			localname = localname + ".txt";
			filename = filename + ".txt";
		}
		else
		{
			localname = localname + "." + format;
			filename = filename + "." + format;
		}
		cout <<log_prefix<< "filename: " << filename << endl;
		if(format == "html")
		{
			outfile.open(localname);
			outfile << success;
			outfile.close();
			if(rs.ansNum <= 100)
			{
				//!Notice: remember to set no-cache in the response of query, Firefox and chrome works well even if you don't set, but IE will act strange if you don't set
				//beacause IE will defaultly cache the query result after first query request, so the following query request of the same url will not be send if the result in cache isn't expired.
				//then the following query will show the same result without sending a request to let the service run query
				//so the download function will go wrong because there is no file in the service.
				*response << "HTTP/1.1 200 OK\r\nContent-Length: " << ansNum_s.length()+filename.length()+success.length()+4;
				*response << "\r\nContent-Type: text/plain";
				*response << "\r\nCache-Control: no-cache" << "\r\nPragma: no-cache" << "\r\nExpires: 0";
				*response  << "\r\n\r\n" << "0+" << rs.ansNum << '+' << filename << '+' << success;
				pthread_rwlock_unlock(&database_load_lock);
				//return true;
				return;
			}
			else
			{
				rs.output_limit = 100;
				success = "";
				success = rs.to_str();
				*response << "HTTP/1.1 200 OK\r\nContent-Length: " << ansNum_s.length()+filename.length()+success.length()+4;
				*response << "\r\nContent-Type: text/plain";
				*response << "\r\nCache-Control: no-cache" << "\r\nPragma: no-cache" << "\r\nExpires: 0";
				*response << "\r\n\r\n" << "1+" << rs.ansNum << '+' << filename << '+' << success;
				pthread_rwlock_unlock(&database_load_lock);
				//return true;
				return;
			}
		}
		else
		{
			
			string filename = "";
			filename = "sparql." + format;
			cout <<log_prefix<< "filename: " << filename << endl;
			*response << "HTTP/1.1 200 OK\r\nContent-Length: " << success.length();
			*response << "\r\nContent-Type: application/octet-stream";
			*response << "\r\nContent-Disposition: attachment; filename=\"" << filename << '"';
			*response << "\r\n\r\n" << success;
			
			//outfile.open(localname);
			//outfile << success;
			//outfile.close();

			//download_result(server, response, request, "true", filename);
			//
			//*response << "HTTP/1.1 200 OK\r\nContent-Length: " << ansNum_s.length()+filename.length()+3;
			//*response << "\r\n\r\n" << "2+" << rs.ansNum << '+' << filename;
			pthread_rwlock_unlock(&database_load_lock);
			return;
			//return true;
		}
	}
	else
	{
		cout <<log_prefix<< "query returned error." << endl;

		string error = "query() returns false.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		//return false;
		return;
	}

	//return true;
}

void query_handler(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	query_num++;
	//cout<<"content: "<<request->content.string()<<endl;
	//cout<<"Header: "<<endl;
	//Usage of auto: http://blog.csdn.net/qq_26399665/article/details/52267734
	//for(auto it : request->header)
	//{
		//cout<<it.first<<" "<<it.second<<endl;
	//}
	//cout<<"client addr: "<<request->remote_endpoint_address<<endl;
	//cout<<"client port: "<<request->remote_endpoint_port<<endl;

	//DEBUG: when using a thread for a query and run in parallism, the QueryParser will fail due to ANTLR
	//In addition, addRequest and io buffer in StringIndex will also cause error
	thread t(&query_thread, response, request);
	t.detach();
	//t.join();

	//DEBUG: here will cause error due to the use of socket in 2 processes
	//NOTICE: we should sue multithreading here because frequently creating/destroying processes will be costly
	//Another reason is the log4cplus supports multithreading but not multiprocessing
	//pid_t child = fork();
	//if(child == -1)
	//{
		//cout<<"child fork error in query processing of ghttp"<<endl;
		////here we do it sequentially
		//query_handler(response, request);
	//}
	//else if(child == 0)
	//{
		//query_handler(response, request);
		////sleep(10); //wait the IO to end in this process
		//cout<<"now the child process to exit"<<endl;
		////DEBUG: if we use this , then error? why? restart endlessly
		////exit(0);
	//}
	//else
	//{
		////if father process exits, all child process will exit
		//pid_t ret = waitpid(child, NULL, 0);
		//if(ret == child)
		//{
			//cout<<"father process get child exit code: "<<ret<<endl;
			////DEBUG: if not exit before, the process won't end, the mmeory of query won't be released
			////TODO: error here
			////return 0;
		//}
		//else
		//{
			//cout<<"error occured"<<endl;
			////return -1;
		//}
	//}
	//The upper is blocked-waiting(the child process should not use exit), 
	//below is non-blocked-waiting(the child process should use exit)
	//do {
		//pid_t ret = waitpid(child, NULL, WNOHANG); 
		//if(ret == 0) {
			//cout<<"xxx"<<endl;
			//sleep(1); //query again for each second
		//}
	//}while(ret == 0);
	//if(ret == child){};
}


void monitor_thread(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<<"HTTP: this is monitor"<<endl;

	if(pthread_rwlock_tryrdlock(&database_load_lock) != 0)
	{
	    string error = "Unable to monitor due to loss of lock";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	if(current_database == NULL)
	{
		string error = "No database used now.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}

	//BETTER: use JSON format to send/receive messages
	//C++ can not deal with JSON directly, JSON2string string2JSON
	string success;
	string name = current_database->getName();
	success = success + "database: " + name + "\n";
	TYPE_TRIPLE_NUM triple_num = current_database->getTripleNum();
	success = success + "triple num: " + Util::int2string(triple_num) + "\r\n";
	TYPE_ENTITY_LITERAL_ID entity_num = current_database->getEntityNum();
	success = success + "entity num: " + Util::int2string(entity_num) + "\r\n";
	TYPE_ENTITY_LITERAL_ID literal_num = current_database->getLiteralNum();
	success = success + "literal num: " + Util::int2string(literal_num) + "\r\n";
	TYPE_ENTITY_LITERAL_ID sub_num = current_database->getSubNum();
	success = success + "subject num: " + Util::int2string(sub_num) + "\r\n";
	TYPE_PREDICATE_ID pre_num = current_database->getPreNum();
	success = success + "predicate num: " + Util::int2string(pre_num) + "\r\n";
	//BETTER: how to compute the connection num in Boost::asio?
	int conn_num = connection_num / 2;
	//int conn_num = 3;    //currectly connected sessions
	//this connection num is countint the total(no break)
	success = success + "connection num: " + Util::int2string(conn_num) + "\r\n";
	//TODO: add the info of memory and thread, operation num and IO frequency

	//success = "<p>" + success + "</p>";
	*response << "HTTP/1.1 200 OK\r\nContent-Length: " << success.length() << "\r\n\r\n" << success;
	pthread_rwlock_unlock(&database_load_lock);
}

bool monitor_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	thread t(&monitor_thread, response, request);
	t.detach();
	return true;
}

bool delete_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	thread t(&delete_thread, response, request);
	t.detach();
	return true;
}

bool download_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	download_thread(server, response, request);
	//thread t(&download_thread, response, request);
	//t.detach();
	return true;
}

void default_thread(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<<"HTTP: this is default"<<endl;

	try {
		auto web_root_path=boost::filesystem::canonical("./Server/web");
		auto path=boost::filesystem::canonical(web_root_path/request->path);
		//Check if path is within web_root_path
		if(distance(web_root_path.begin(), web_root_path.end())>distance(path.begin(), path.end()) ||
				!equal(web_root_path.begin(), web_root_path.end(), path.begin()))
			throw invalid_argument("path must be within root path");
		if(boost::filesystem::is_directory(path))
			path/="index.html";
		if(!(boost::filesystem::exists(path) && boost::filesystem::is_regular_file(path)))
			throw invalid_argument("file does not exist");

		std::string cache_control, etag;
		// Uncomment the following line to enable Cache-Control
		// cache_control="Cache-Control: max-age=86400\r\n";

		auto ifs=make_shared<ifstream>();
		ifs->open(path.string(), ifstream::in | ios::binary | ios::ate);
		string extName = path.extension().string();
		cout << "extName: " << extName << endl;
		
		if(*ifs) {
			auto length=ifs->tellg();
			ifs->seekg(0, ios::beg);

			*response << "HTTP/1.1 200 OK\r\n" << cache_control << etag << "Content-Length: " << length << "\r\n";
			//!Notice: IE need to declare the Content-type of the response, if not the browser will not show the result with an unknow format.
			//it works well in Firefox and Chrome if you don't declare the Content-type, but in IE the css file will be ignored because of unmatched MIME type, then the web page will show in a form without css style and it looks ugly.
			if(extName == ".html")
				*response << "Content-Type: text/html" << "\r\n\r\n";
			else if(extName == ".js")
				*response << "Content-Type: application/x-javascript" << "\r\n\r\n";
			else if(extName == ".css")
				*response << "Content-Type: text/css" << "\r\n\r\n";
			else if(extName == ".png")
				*response << "Content-Type: image/png" << "\r\n\r\n";
			else if(extName == ".jpg")
				*response << "Content-Type: image/jpeg" << "\r\n\r\n";
			default_resource_send(server, response, ifs);
		}
		else
			throw invalid_argument("could not read file");
	}
	catch(const exception &e) {
		string content="Could not open path "+request->path+": "+e.what();
		*response << "HTTP/1.1 400 Bad Request\r\nContent-Length: " << content.length() << "\r\n\r\n" << content;
	}
}

bool default_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	connection_num++;
	string req_url = request->path;
	cout << "request url: " << req_url << endl;
	//if request matches /?operation=monitor, then do monitor_handler
	//because if the user visit through the browser by using url /?operation=monitor
	//it can not match directly to monitor_handler, and will match this default get
	//so we need to check here to do monitor_handler, although the implementation is not perfect enough.
	if(req_url == "/?operation=monitor")
	{
		monitor_handler(server, response, request);
		return true;
	}

	default_thread(server, response, request);
	//thread t(&default_thread, response, request);
	//t.detach();
	return true;
}

void checkpoint_thread(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<<"HTTP: this is checkpoint"<<endl;

	if(pthread_rwlock_tryrdlock(&database_load_lock) != 0)
	{
		string error = "Unbale to checkpoint due to the loss of lock.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	if(current_database == NULL)
	{
		string error = "No database used.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}

	//For database checkpoint or log/transaction:
	//http://www.newsmth.net/nForum/#!article/LinuxDev/37802?p=2
	//http://blog.csdn.net/cywosp/article/details/8767327
	//http://www.fx114.net/qa-63-143449.aspx
	current_database->save();
	//NOTICE: this info is in header
	string success = "Database saveed successfully.";
	//header and content are split by an empty line
	*response << "HTTP/1.1 200 OK\r\nContent-Length: " << success.length() << "\r\n\r\n" << success;

	pthread_rwlock_unlock(&database_load_lock);
}

//TODO+BETTER: server choose to save a database when system is not busy
//If user send this command too frequently, the performance may be awful if updates are large
bool checkpoint_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	thread t(&checkpoint_thread, response, request);
	t.detach();
	return true;
}

void show_thread(const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	string thread_id = Util::getThreadID();
	string log_prefix = "thread " + thread_id + " -- ";
	cout<<log_prefix<<"HTTP: this is show"<<endl;

	if(pthread_rwlock_tryrdlock(&database_load_lock) != 0)
	{
	    string error = "Unable to show due to loss of lock";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		return;
	}

	if(current_database == NULL)
	{
		string error = "No database used.";
		*response << "HTTP/1.1 200 OK\r\nContent-Length: " << error.length() << "\r\n\r\n" << error;
		pthread_rwlock_unlock(&database_load_lock);
		return;
	}

	//NOTICE: this info is in header
	string success = current_database->getName();
	*response << "HTTP/1.1 200 OK\r\nContent-Length: " << success.length() << "\r\n\r\n" << success;
	pthread_rwlock_unlock(&database_load_lock);
}

//BETTER: indicate the db_name when query
bool show_handler(const HttpServer& server, const shared_ptr<HttpServer::Response>& response, const shared_ptr<HttpServer::Request>& request)
{
	thread t(&show_thread, response, request);
	t.detach();
	return true;
}


