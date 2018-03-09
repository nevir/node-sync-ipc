#include <node.h>
#include <uv.h>
#include <nan.h>
#include <string.h>
#include "basic.h"

namespace client {

    using v8::FunctionCallbackInfo;
    using v8::Isolate;
    using v8::Local;
    using v8::Object;
    using v8::String;
    using v8::Value;
    using v8::Number;

    char * returnValue;

    char * writeValue;

    char * pipename;

    int readFullLen;

    uv_pipe_t* client_handle;

    uv_loop_t * ipc_loop = NULL;

    int pid;

    int pid_digits;

    int parent_pid;

    int writingOffset = 0;

    void write();

    int getIntDigits(int num){

        int digits = 0;

        while(num>0){
           digits += 1;
           num /= 10;
        }

        return digits;
    }

    typedef struct {
        uv_write_t req;
        uv_buf_t buf;
    } write_req_t;

    const char* ToCString(const String::Utf8Value& value) {
          return *value ? *value : "<string conversion failed>";
    }


    void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
      buf->base = (char *) malloc(suggested_size);
      buf->len = suggested_size;
    }

    void free_write_req(uv_write_t *req) {
        write_req_t *wr = (write_req_t*) req;
        free(wr->buf.base);
        free(wr);
    }

    void on_client_closed(uv_handle_t * client){

        free(client);
    }

    void getPidAndMessage(char* raw, int * pid, char ** message){


            int count = strlen(raw);
            int i = 0;
            while(raw[i] != '#' && raw[i]){
                i++;
                if(i >= count){
                    break;
                }
            }

            if(i >= count){
                return;
            }

            char * pidSub = (char *) malloc(sizeof(char) * (i+1));
            memcpy( pidSub,raw, i );
            pidSub[i] = '\0';
            *pid = atoi(pidSub);
            free(pidSub);

            *message = (char *) malloc(sizeof(char) * (strlen(raw)-i));
            memcpy(*message,raw+i+1,strlen(raw)-i-1);
            (*message)[strlen(raw)-i-1] = '\0';

    }

    void on_read_value(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {

        if (nread > 0) {

            char * buffer = (char *) malloc(nread+1);
            memcpy(buffer,buf->base,nread);
            buffer[nread] = 0;

            if(DEBUG) fprintf(stdout,"Client Read %ld: %s \n",nread,buffer);

            char *body;

            if(returnValue == NULL){
                getPidAndMessage(buffer,&readFullLen,&body);
                returnValue = (char *) malloc(readFullLen+1);
                for(int i = 0; i <  readFullLen + 1; i++){
                    returnValue[i] = 0;
                }
                free(buffer);
            }
            else{
                body = buffer;
            }

            strcat(returnValue, body);

            free(body);

            if(DEBUG) fprintf(stdout,"client received %d / %d \n", strlen(returnValue), readFullLen);

            if(strlen(returnValue) == readFullLen){
                if(DEBUG) fprintf(stdout,"Client Read Finished %ld: %s \n",nread,returnValue);

                uv_read_stop(client);
                uv_close((uv_handle_t *) client,on_client_closed);
            }
        }

        if (nread < 0) {
            if (nread != UV_EOF)
                if(DEBUG) fprintf(stderr, "Read error %s\n", uv_err_name(nread));
            uv_close((uv_handle_t*) client, on_client_closed);
        }

        free(buf->base);
    }

    void write_cb(uv_write_t* req, int status) {

       if (status != 0) {
               if(DEBUG) fprintf(stderr, "Client Write error %s\n", uv_err_name(status));
       }
       free_write_req(req);


       int fullLength = strlen(writeValue);


       if(writingOffset < fullLength){
            write();
       }
       else{
            free(writeValue);
        }
    }

    void write(){
        int fullLength = strlen(writeValue);
        int finished = 0;
        if( fullLength-writingOffset <= MAX_BUFFER_LENGTH){
            finished = 1;
        }
        int len = (finished)? fullLength-writingOffset : MAX_BUFFER_LENGTH;


        char *buffer;
        if(writingOffset == 0){
             int lenDigits = getIntDigits(fullLength);
             buffer = (char *) malloc(sizeof(char)*(pid_digits+1+lenDigits+1+len+1));
             sprintf(buffer,"%d#%d#",pid,fullLength);
             memcpy(buffer+pid_digits+1+lenDigits+1,writeValue+writingOffset,len);
             buffer[pid_digits+1+lenDigits+1+len] = 0;
        }
        else{
            buffer = (char *) malloc(sizeof(char)*(len+1));
            memcpy(buffer,writeValue+writingOffset,len);
            buffer[len] = 0;
        }
        writingOffset += len;

        if(DEBUG) fprintf(stdout,"client write %s \n", buffer);

        write_req_t *wreq = (write_req_t*) malloc(sizeof(write_req_t));

        wreq->buf = uv_buf_init(buffer, strlen(buffer));

        uv_write(&wreq->req, (uv_stream_t *)client_handle, &wreq->buf, 1, write_cb);
    }

    void on_client_connected(uv_connect_t* req, int status){

        if (status != 0) {
            // error!
            if(DEBUG) fprintf(stdout,"client connection error %s\n", uv_err_name(status));

        }

        else{

            if(DEBUG) fprintf(stdout,"client connected %d \n", status);

            returnValue = NULL;

            uv_read_start((uv_stream_t*) req->handle, alloc_buffer, on_read_value);

            writingOffset = 0;

            write();

        }


        free(req);

    }

    void connect(){

           ipc_loop = (uv_loop_t *) malloc(sizeof(uv_loop_t));

           uv_loop_init(ipc_loop);

           uv_connect_t* req = (uv_connect_t *) malloc(sizeof(uv_connect_t));

           client_handle = (uv_pipe_t *) malloc(sizeof(uv_pipe_t));

           uv_pipe_init(ipc_loop, client_handle,0);



           uv_pipe_connect(req, client_handle, pipename, on_client_connected);

           uv_run(ipc_loop,UV_RUN_DEFAULT);

           if(DEBUG) fprintf(stdout,"connect loop successfully closed\n");

           uv_loop_close(ipc_loop);

           free(ipc_loop);

        }

    NAN_METHOD(setPid){
        if (info.Length() > 0) {
            if (info[0]->IsNumber()) {

                int parent_pid = (int) Local<Number>::Cast(info[0])->NumberValue();
                int temp = parent_pid;
                int parent_pid_digits = 0;
                while(temp>0){
                   parent_pid_digits += 1;
                   temp /= 10;
                }
                #ifdef _WIN32
                const char* pipename_preset = "\\\\.\\pipe\\nodePipe";
                pipename = (char *) malloc(sizeof(char) * (strlen(pipename_preset)+parent_pid_digits+1));
                sprintf(pipename,"%s%d",pipename_preset,parent_pid);
                #else
                const char *homedir;
                if ((homedir = getenv("HOME")) == NULL) {
                    homedir = getpwuid(getuid())->pw_dir;
                }
                const char* pipename_preset = "nodePipe";
                pipename = (char *) malloc(sizeof(char) * (strlen(pipename_preset)+parent_pid_digits+strlen(homedir)+7));
                sprintf(pipename,"/%s/%s%d.sock",homedir,pipename_preset,parent_pid);
                #endif

            }
        }
    }

    NAN_METHOD(send){

        if (info.Length() > 0) {

            if (info[0]->IsString()) {

                  String::Utf8Value str(info[0]->ToString());

                  const char* s = ToCString(str);

                  writeValue = strdup(s);

                  if(DEBUG) fprintf(stdout, "send Sync length %d \n", strlen(writeValue) );

                  connect();

                  if(returnValue != NULL){
                    info.GetReturnValue().Set(Nan::New<v8::String>(returnValue).ToLocalChecked());
                    free(returnValue);

                    returnValue = NULL;

                    if(DEBUG) fprintf(stdout, "send Sync result success \n");
                    return;
                  }
                  else{
                    Nan::ThrowError("Failed To Send Sync");
                  }

            }
        }

        info.GetReturnValue().Set(Nan::Null());


    }

    int _getpid(){
        #ifdef _WIN32
        return GetCurrentProcessId();
        #else
        return getpid();
        #endif
    }

    NAN_MODULE_INIT(init) {
      Nan::SetMethod(target, "sendSync", send);
      Nan::SetMethod(target, "setParentPid", setPid);

        pid = _getpid();

        pid_digits = 0;

        int temp = pid;

        while(temp>0){
           pid_digits += 1;
           temp /= 10;
        }
    }

    NODE_MODULE(NODE_GYP_MODULE_NAME, init)



}




