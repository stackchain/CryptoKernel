/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    httpserver.cpp
 * @date    31.12.2012
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>,
 *          James Lovejoy <jameslovejoy1@gmail.com>
 * @license See libjson-rpc-cpp for MIT license
 ************************************************************************/

#include "httpserver.h"
#include <cstdlib>
#include <cstring>
#include <sstream>

#ifdef __APPLE__
#include <netinet/in.h>
#elif __unix__
#include <netinet/in.h>
#endif


#include <jsonrpccpp/common/specificationparser.h>

using namespace jsonrpc;
using namespace std;

#define BUFFERSIZE 65536

struct mhd_coninfo {
        struct MHD_PostProcessor *postprocessor;
        MHD_Connection* connection;
        stringstream request;
        HttpServerLocal* server;
        int code;
};

HttpServerLocal::HttpServerLocal(int port, const std::string& username, const std::string& password,
								 const std::string &sslcert, const std::string &sslkey,
								 int threads) :
    AbstractServerConnector(),
    port(port),
    threads(threads),
    running(false),
    path_sslcert(sslcert),
    path_sslkey(sslkey),
	username(username),
	password(password),
    daemon(NULL)
{
}

IClientConnectionHandler *HttpServerLocal::GetHandler(const std::string &url)
{
    if (AbstractServerConnector::GetHandler() != NULL)
        return AbstractServerConnector::GetHandler();
    map<string, IClientConnectionHandler*>::iterator it = this->urlhandler.find(url);
    if (it != this->urlhandler.end())
        return it->second;
    return NULL;
}

bool HttpServerLocal::StartListening()
{
    if(!this->running)
    {
        const bool has_epoll = (MHD_is_feature_supported(MHD_FEATURE_EPOLL) == MHD_YES);
        const bool has_poll = (MHD_is_feature_supported(MHD_FEATURE_POLL) == MHD_YES);
        unsigned int mhd_flags;
        if (has_epoll)
            mhd_flags = MHD_USE_EPOLL_INTERNALLY;
        else if (has_poll)
            mhd_flags = MHD_USE_POLL_INTERNALLY;
        else
            mhd_flags = MHD_USE_SELECT_INTERNALLY;
        if (this->path_sslcert != "" && this->path_sslkey != "")
        {
            try {
                SpecificationParser::GetFileContent(this->path_sslcert, this->sslcert);
                SpecificationParser::GetFileContent(this->path_sslkey, this->sslkey);

                this->daemon = MHD_start_daemon(MHD_USE_SSL | mhd_flags, this->port, HttpServerLocal::accessCallback, NULL, HttpServerLocal::callback, this, MHD_OPTION_HTTPS_MEM_KEY, this->sslkey.c_str(), MHD_OPTION_HTTPS_MEM_CERT, this->sslcert.c_str(), MHD_OPTION_THREAD_POOL_SIZE, this->threads, MHD_OPTION_END);
            }
            catch (JsonRpcException& ex)
            {
                return false;
            }
        }
        else
        {
            this->daemon = MHD_start_daemon(mhd_flags, this->port, HttpServerLocal::accessCallback, NULL, HttpServerLocal::callback, this,   MHD_OPTION_THREAD_POOL_SIZE, this->threads, MHD_OPTION_END);
        }
        if (this->daemon != NULL)
            this->running = true;

    }
    return this->running;
}

bool HttpServerLocal::StopListening()
{
    if(this->running)
    {
        MHD_stop_daemon(this->daemon);
        this->running = false;
    }
    return true;
}

bool HttpServerLocal::SendResponse(const string& response, void* addInfo)
{
    struct mhd_coninfo* client_connection = static_cast<struct mhd_coninfo*>(addInfo);
    struct MHD_Response *result = MHD_create_response_from_buffer(response.size(),(void *) response.c_str(), MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(result, "Content-Type", "application/json");
    MHD_add_response_header(result, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(client_connection->connection, client_connection->code, result);
    MHD_destroy_response(result);
    return ret == MHD_YES;
}

bool HttpServerLocal::SendOptionsResponse(void* addInfo)
{
    struct mhd_coninfo* client_connection = static_cast<struct mhd_coninfo*>(addInfo);
    struct MHD_Response *result = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(result, "Allow", "POST, OPTIONS");
    MHD_add_response_header(result, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(result, "Access-Control-Allow-Headers", "origin, content-type, accept, authorization");
    MHD_add_response_header(result, "DAV", "1");

    int ret = MHD_queue_response(client_connection->connection, client_connection->code, result);
    MHD_destroy_response(result);
    return ret == MHD_YES;
}

void HttpServerLocal::SetUrlHandler(const string &url, IClientConnectionHandler *handler)
{
    this->urlhandler[url] = handler;
    this->SetHandler(NULL);
}

int HttpServerLocal::callback(void *cls, MHD_Connection *connection, const char *url, const char *method, const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls)
{
    (void)version;
    if (*con_cls == NULL)
    {
        struct mhd_coninfo* client_connection = new mhd_coninfo;
        client_connection->connection = connection;
        client_connection->server = static_cast<HttpServerLocal*>(cls);
        *con_cls = client_connection;
        return MHD_YES;
    }
    struct mhd_coninfo* client_connection = static_cast<struct mhd_coninfo*>(*con_cls);

	char* password = NULL;
	char* username = MHD_basic_auth_get_username_password(client_connection->connection,
														  &password);
                              
  if (string("POST") == method)
  {
    if(password != NULL && username != NULL) {
        if(strcmp(password, client_connection->server->password.c_str()) == 0
        && strcmp(username, client_connection->server->username.c_str()) == 0) {
            if (*upload_data_size != 0)
            {
              client_connection->request.write(upload_data, *upload_data_size);
              *upload_data_size = 0;
              return MHD_YES;
            }
            else
            {
              string response;
              IClientConnectionHandler* handler = client_connection->server->GetHandler(string(url));
              if (handler == NULL)
              {
                client_connection->code = MHD_HTTP_INTERNAL_SERVER_ERROR;
                client_connection->server->SendResponse("No client connection handler found", client_connection);
              }
              else
              {
                client_connection->code = MHD_HTTP_OK;
                handler->HandleRequest(client_connection->request.str(), response);
                client_connection->server->SendResponse(response, client_connection);
              }
            }
        } else {
          client_connection->code = MHD_HTTP_UNAUTHORIZED;
          client_connection->server->SendResponse("Username or password incorrect", client_connection);
        }
    } else {
      client_connection->code = MHD_HTTP_UNAUTHORIZED;
      client_connection->server->SendResponse("You must authenticate", client_connection);
    }
  } else if (string("OPTIONS") == method) {
    client_connection->code = MHD_HTTP_OK;
    client_connection->server->SendOptionsResponse(client_connection);
  } else {
    client_connection->code = MHD_HTTP_METHOD_NOT_ALLOWED;
    client_connection->server->SendResponse("Not allowed HTTP Method", client_connection);
  }

	if(username != NULL) {
		free(username);
	}

	if(password != NULL) {
		free(password);
	}

    delete client_connection;
    *con_cls = NULL;

    return MHD_YES;
}

int HttpServerLocal::accessCallback(void *cls, const struct sockaddr* addr, socklen_t addrlen) {
    if((*(sockaddr_in*)addr).sin_addr.s_addr != 0x100007f) {
        return MHD_NO;
    }

    return MHD_YES;
}

