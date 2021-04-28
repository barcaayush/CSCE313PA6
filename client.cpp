#include "common.h"
#include "BoundedBuffer.h"
#include "Histogram.h"
#include "common.h"
#include "HistogramCollection.h"
#include "TCPreqchannel.h"
#include <thread>
#include <signal.h>
#include <getopt.h>
#include <vector>
#include <algorithm>
#include <unistd.h>
//#include <sys/wait.h>
#include <sys/epoll.h>
#include <unordered_map>
#include <fcntl.h>

static HistogramCollection* bonusPart;
using namespace std;

int k = 0;

class Response{

public:
    int person;
    double ecgVal;

    Response(int pno, double ecg){
        person = pno;
        ecgVal = ecg;
    }
};


void display_alarm(int sig)
{
    system("clear");
    //printing the histogram collection on every alarm
    bonusPart->print();
    //setting up alarm every second
    alarm(2);
    //getting the signal 
    signal(SIGALRM, display_alarm);
}

void patient_thread_function(int pno, int n, BoundedBuffer* req_buf){
    double time = 0.000;
    for(int i = 0; i < n; i++){
        datamsg * dm = new datamsg(pno, time, 1);
        req_buf->push(((char*) dm), sizeof(datamsg));
        time += 0.004;
        delete dm;
    }
}

void * event_polling_thread(/*TCPRequestChannel ** chan*/vector<TCPRequestChannel*> chan, BoundedBuffer* req_buf, int capacity, BoundedBuffer * resp, int w)
{

    char buf[1024];
    char receivebuf[capacity];
    struct epoll_event ev;
    struct epoll_event ev_lists[w];

    int epoll_file_disc = epoll_create1(0);

    if(epoll_file_disc == -1)
    {
        EXITONERROR("cannot create epoll list");
    }



    vector<vector<char>> state(w);
    unordered_map<int, int> fds;
    int numSent = 0;
    int numReceive = 0;

  

    for(int i = 0; i < w; i++)
    {
        int size = req_buf->pop(buf, 1024);
        chan[i]->cwrite(buf, size);
        numSent++;
        state[i] = vector<char> (buf, buf+size);
        int rfd = chan[i]->get_socket_fd();
        fds[rfd] = i;
        fcntl(rfd, F_SETFL, O_NONBLOCK);
        ev.events = EPOLLIN|EPOLLET;
        ev.data.fd = rfd;
        if(epoll_ctl(epoll_file_disc, EPOLL_CTL_ADD, rfd, &ev) == -1)
        {

        }
    }


    bool quitRecv = false;

    while(true)
    {
        if(quitRecv == true && numReceive == numSent )
            {
                break;
            }
        int num_file_disc = epoll_wait(epoll_file_disc, ev_lists, w, -1);   //number of file discriptor 
        if(num_file_disc == -1)
        {
            EXITONERROR("error on epoll waiting");
        }

        for(int i = 0; i < num_file_disc; i++)
        {
            
            int read_fd = ev_lists[i].data.fd;
            int idx = fds[read_fd];
            int response_size  = chan[idx]->cread(&buf, capacity);
            numReceive++;
            vector<char> request = state[idx];
            char* req = request.data(); 

            MESSAGE_TYPE *m = (MESSAGE_TYPE*) req;
            if(*m == DATA_MSG)
            {
                datamsg * msg = (datamsg *) req;
                double ecg = * (double *) buf;
                Response res(msg->person, ecg);
                resp->push((char *) &res, sizeof(Response));
            }
            else if(*m == FILE_MSG)
            {
                filemsg* fmsg = (filemsg *) req;
                string fileName = (char *) (fmsg + 1);
                int size = sizeof(filemsg) + fileName.size() + 1;
                string rFile = "received/" + fileName;
                FILE *filePointer = fopen(rFile.c_str(), "r+");
                fseek(filePointer, fmsg->offset, SEEK_SET);
                fwrite(buf, 1, fmsg->length, filePointer);
                fclose(filePointer);
            }
            
            if(!quitRecv)
            {
                int request_size = req_buf->pop(buf, sizeof(buf));
                chan[idx]->cwrite(buf, request_size);
                numSent++;
                state[idx] = vector<char> (buf, buf+request_size);
                if(*(MESSAGE_TYPE *) buf == QUIT_MSG)
                {
                    quitRecv = true;
                } 
               
            }
            
        }
    }
}


void * histogram_thread_function(HistogramCollection* hc, BoundedBuffer* res_buf){
    char buf[1024];
    while(true)
    {
        res_buf->pop(buf, 1024);
        Response * resp = (Response *) buf;
        if(resp->person == -1)
        {
            break;
        }
        hc->updateHistogram(resp->person, resp->ecgVal);
    }
}

void * file_thread(TCPRequestChannel * chan, string fileName, BoundedBuffer* req_buf, int size)
{
    char buffer[1024];
    filemsg fmsg(0, 0);
    memcpy(buffer, &fmsg, sizeof(fmsg));
    strcpy(buffer + sizeof(fmsg), fileName.c_str());
    chan->cwrite(buffer, sizeof(fmsg) + fileName.size() + 1);
    __int64_t fileLength;
    chan->cread(&fileLength, sizeof(fileLength));

    string recvFile = "received/" + fileName;
    FILE *fp = fopen(recvFile.c_str(), "w");
    fseek(fp, fileLength, SEEK_SET);
    fclose(fp);

    filemsg *fm =(filemsg*) buffer;
    __int64_t remainingLen = fileLength;

    while(remainingLen > 0)
    {
        fm->length = min(remainingLen, (__int64_t)size);
        req_buf->push(buffer, sizeof(filemsg)+fileName.size()+1);
        fm->offset += fm->length;
        remainingLen -= fm->length;
    }
}


int main(int argc, char *argv[])
{
    int n = 1000;    //default number of requests per "patient"
    int p = 10;     // number of patients [1,15]
    int w = 10;    //default number of worker threads
    int b = 1024; 	// default capacity of the request buffer, you should change this default
	int m = MAX_MESSAGE; 	// default capacity of the message buffer
    int h = 10;   //default histogram thread
    string host, port;
    srand(time_t(NULL));  
    string fileName = "";
    bool isFile = false;
    int capacity = MAX_MESSAGE;
    char* argument = (char*)to_string(capacity).c_str();
    int opt;   

        while((opt = getopt(argc, argv, "n:p:w:b:f:m:h:r:")) != -1)  {  
            switch(opt)  {  
                case 'n':  
                    n = atoi(optarg);
                    break;  
                case 'p':  
                    p = atoi(optarg);  
                    break;  
                case 'w': 
                    w = atoi(optarg); 
                    break;
                case 'b':
                    b = atoi(optarg); 
                    break;
                case 'f':
                    isFile = true;
                    fileName = optarg;
                    break;
                case 'm':
                    capacity = atoi(optarg);
                    argument = (char *)to_string(capacity).c_str();
                    break; 
                case 'h':
                    host = optarg;
                    break;
                case 'r':
                    port = optarg;
                    break;
                case '?':
                    cout<<"Invalid input"<<endl;
                    break;
            } 
        } 
    

    //creating the request buffer;
    BoundedBuffer request_buffer(b);
    //creating the response buffer;
    BoundedBuffer response_buffer(b);
	HistogramCollection hc;
    

    //TCPRequestChannel ** wchannels = new TCPRequestChannel* [w];
    vector<TCPRequestChannel*> wchannels(w);
    for(int i = 0; i < w; i++)
    {
        wchannels[i] = new TCPRequestChannel(host, port);
    }	


    //Getting patient thread 
    if(!isFile){
        struct timeval start, end;
        gettimeofday (&start, 0);
        //adding histogram on histogram collection
        Histogram* histo;
        for(int i = 0; i < p; i ++){
            histo = new Histogram(15, -2.0, 2.0);
            hc.add(histo);
        }
        //adding alarm signals 
        bonusPart = &hc;
        signal(SIGALRM, display_alarm);
        alarm(2);
        
        vector<thread> patient_t;
        for(int i = 0; i < p; i++)
        {
            patient_t.push_back(thread(patient_thread_function, i+1, n, &request_buffer)); 
        }

        thread event_polling(thread(event_polling_thread, wchannels, &request_buffer, capacity, &response_buffer, w));

        vector<thread> hist;
        for(int i = 0; i < h; i++)
        {
            hist.push_back(thread(histogram_thread_function, &hc, &response_buffer));
        }

        //joining the patient thread
        for(int i = 0; i < p; i++)
        {
            patient_t[i].join();
        }
        MESSAGE_TYPE qm = QUIT_MSG;
        request_buffer.push((char *) &qm, sizeof(MESSAGE_TYPE));
        event_polling.join();

        //hc.print();
    
    

        Response* quit = new Response(-1, 0);
        for(int i = 0; i < h; i++)
        {
            //pushing the quit message to the response buffer
            response_buffer.push((char *) quit, sizeof(quit));
        }
        
        delete quit;

    
        //joining the histogram thread
        for(int i = 0; i < h; i++)
        {
        //    
            hist[i].join();
        }

        //delete wchannels;
        gettimeofday (&end, 0);
        // system("clear");
        hc.print();
        int secs = (end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)/(int) 1e6;
        int usecs = (int)(end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)%((int) 1e6);
        cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;

        cout << "All Done!!!" << endl;

        for(int i = 0; i < w; i++)
        {
            wchannels[i]->cwrite((char *) &qm, sizeof(MESSAGE_TYPE));
            delete wchannels[i];
            //cout << "quitting" << endl;
        }

        //delete []wchannels;
        
        cout<<sizeof(Histogram)<<endl;
        cout << sizeof(TCPRequestChannel) << endl;
        return 0;

    }
	else if(isFile) {
        struct timeval start, end;
        gettimeofday (&start, 0);

        cout << "File thread" << endl;
        thread fileThread(file_thread, wchannels[0], fileName, &request_buffer, capacity);

        cout << "Event polling is creagted " << endl;
        thread event_polling(thread(event_polling_thread, wchannels, &request_buffer, capacity, &response_buffer, w));

       fileThread.join();

        cout << "Quiting event thread" << endl;
        MESSAGE_TYPE qm = QUIT_MSG;
        request_buffer.push((char *) &qm, sizeof(MESSAGE_TYPE));
        event_polling.join();

        for(int i = 0; i < w; i++)
        {
            // wchannels[i]->cwrite((char *) &qm, sizeof(MESSAGE_TYPE));
            delete wchannels[i];
        }

        //delete[] wchannels;


        gettimeofday (&end, 0);
        int secs = (end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)/(int) 1e6;
        int usecs = (int)(end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)%((int) 1e6);
        cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;
        cout << "All Done!!!" << endl;
    }

}
