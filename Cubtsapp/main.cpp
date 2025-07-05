#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <map>
#include <algorithm>
#include <cctype>
#include <sstream>  
#include <string>   
#pragma comment(lib, "ws2_32.lib")

using namespace std;


const int PORT = 12345;


enum class Mode { SERVER, CLIENT };


mutex clientsMutex;
vector<SOCKET> clients;
map<SOCKET, string> clientLogins;


namespace Plugins {
    
    string GetCurrentTime() {
        auto now = chrono::system_clock::now();
        time_t now_time = chrono::system_clock::to_time_t(now);
        tm now_tm;
        localtime_s(&now_tm, &now_time);  
        stringstream ss;
        ss << put_time(&now_tm, "%H:%M:%S");
        return ss.str();
    }

    
    bool Authenticate(const string& login, const string& password) {
        static const map<string, string> credentials = {
            {"Test", "Test"}, // Login and password
            {"Test1", "Test1"},
        };

        auto it = credentials.find(login);
        return it != credentials.end() && it->second == password;
    }

    
    string ResolveDNS(const string& hostname) {
        addrinfo hints, * result;
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        if (getaddrinfo(hostname.c_str(), NULL, &hints, &result) != 0) {
            return "";
        }

        char ipStr[INET_ADDRSTRLEN];
        sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(result->ai_addr);
        inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));

        freeaddrinfo(result);
        return string(ipStr);
    }
}


void broadcastMessage(const string& message, SOCKET sender = INVALID_SOCKET) {
    lock_guard<mutex> lock(clientsMutex);
    for (SOCKET client : clients) {
        if (client != sender) {
            send(client, message.c_str(), static_cast<int>(message.length()), 0);
        }
    }
}

void handleClient(SOCKET clientSocket) {
    char buffer[1024];
    int bytesReceived;

    
    string loginPrompt = "LOGIN: ";
    send(clientSocket, loginPrompt.c_str(), static_cast<int>(loginPrompt.length()), 0);

    bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        closesocket(clientSocket);
        return;
    }
    buffer[bytesReceived] = '\0';
    string login(buffer);

    string passwordPrompt = "PASSWORD: ";
    send(clientSocket, passwordPrompt.c_str(), static_cast<int>(passwordPrompt.length()), 0);

    bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        closesocket(clientSocket);
        return;
    }
    buffer[bytesReceived] = '\0';
    string password(buffer);

    
    if (!Plugins::Authenticate(login, password)) {
        string authFail = "AUTH_FAIL\n";
        send(clientSocket, authFail.c_str(), static_cast<int>(authFail.length()), 0);
        closesocket(clientSocket);
        return;
    }

    string authOk = "AUTH_OK\n";
    send(clientSocket, authOk.c_str(), static_cast<int>(authOk.length()), 0);

    
    {
        lock_guard<mutex> lock(clientsMutex);
        clients.push_back(clientSocket);
        clientLogins[clientSocket] = login;
    }

    string joinMsg = login + " joined the chat.\n";
    broadcastMessage(joinMsg);

    
    while (true) {
        bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) break;

        buffer[bytesReceived] = '\0';
        string message(buffer);

        if (message == "exit") break;

        
        string formattedMsg = "[" + Plugins::GetCurrentTime() + "] " + login + ": " + message + "\n";
        broadcastMessage(formattedMsg, clientSocket);
    }

    
    {
        lock_guard<mutex> lock(clientsMutex);
        auto it = find(clients.begin(), clients.end(), clientSocket);
        if (it != clients.end()) clients.erase(it);
        clientLogins.erase(clientSocket);
    }

    string leaveMsg = login + " left the chat.\n";
    broadcastMessage(leaveMsg);
    closesocket(clientSocket);
}

void runServer() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "WSAStartup failed." << endl;
        return;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        cerr << "Socket creation failed." << endl;
        WSACleanup();
        return;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        cerr << "Bind failed." << endl;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        cerr << "Listen failed." << endl;
        closesocket(serverSocket);
        WSACleanup();
        return;
    }

    cout << "Server started on port " << PORT << ". Waiting for connections..." << endl;

    while (true) {
        SOCKET clientSocket = accept(serverSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) {
            cerr << "Accept failed." << endl;
            continue;
        }

        thread clientThread(handleClient, clientSocket);
        clientThread.detach();
    }

    closesocket(serverSocket);
    WSACleanup();
}


void receiveMessages(SOCKET sock) {
    char buffer[1024];
    while (true) {
        int bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) {
            cout << "Disconnected from server." << endl;
            break;
        }
        buffer[bytesReceived] = '\0';
        cout << buffer;
    }
}

void runClient() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "WSAStartup failed." << endl;
        return;
    }

    string serverInput;
    cout << "Enter server IP or hostname: ";
    getline(cin, serverInput);

    string serverIP;
    
    if (serverInput.find_first_not_of("0123456789.") != string::npos) {
        serverIP = Plugins::ResolveDNS(serverInput);
        if (serverIP.empty()) {
            cerr << "DNS resolution failed. Using as IP." << endl;
            serverIP = serverInput;
        }
    }
    else {
        serverIP = serverInput;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        cerr << "Socket creation failed." << endl;
        WSACleanup();
        return;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        cerr << "Connect failed to " << serverIP << ":" << PORT << endl;
        closesocket(sock);
        WSACleanup();
        return;
    }

    char buffer[1024];
    int bytesReceived;

    
    bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        closesocket(sock);
        WSACleanup();
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << buffer;

    string login;
    getline(cin, login);
    send(sock, login.c_str(), static_cast<int>(login.length()), 0);

    
    bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        closesocket(sock);
        WSACleanup();
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << buffer;

    string password;
    getline(cin, password);
    send(sock, password.c_str(), static_cast<int>(password.length()), 0);

    
    bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        closesocket(sock);
        WSACleanup();
        return;
    }
    buffer[bytesReceived] = '\0';
    string authResponse(buffer);

    if (authResponse != "AUTH_OK\n") {
        cout << "Authentication failed. Disconnecting..." << endl;
        closesocket(sock);
        WSACleanup();
        return;
    }

    cout << "Authentication successful. Enter messages (type 'exit' to quit):" << endl;

    thread receiverThread(receiveMessages, sock);
    receiverThread.detach();

    
    string message;
    while (true) {
        getline(cin, message);
        send(sock, message.c_str(), static_cast<int>(message.length()), 0);
        if (message == "exit") break;
    }

    closesocket(sock);
    WSACleanup();
}

int main() {
    cout << "Run as server or client? (s/c): ";
    string choice;
    getline(cin, choice);

    transform(choice.begin(), choice.end(), choice.begin(), ::tolower);

    if (choice == "s" || choice == "server") {
        runServer();
    }
    else if (choice == "c" || choice == "client") {
        runClient();
    }
    else {
        cout << "Invalid choice. Exiting." << endl;
    }

    return 0;
}
