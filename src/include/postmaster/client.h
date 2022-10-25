#ifndef _TAAS_CLIENT_H
#define _TAAS_CLIENT_H
//ADDBY TAAS
#include <thread>
#include <vector>
extern std::vector<uint64_t> 
    client_listener_thread_ids, client_sender_thread_ids, client_manager_thread_ids;


extern void ClientListenerMain(void);
extern void ClientSenderMain(void);
extern void ClientManagerMain(void);

#endif