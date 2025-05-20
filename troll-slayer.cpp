#include <mpi.h>

#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>

#define NUM_CITIES 3             // Liczba miast
#define MAX_TIME_IN_CITY 20      // Maksymalny czas przebywania w mieście w sekundach
#define MAX_TIME_DOING_NOTHING 5 // Maksymalny czas nicnierobienia w sekundach
#define TAG 0

typedef enum
{
    REQ,
    ACK
} MessageType;

typedef struct
{
    MessageType type;
    int city_id;
    int timestamp;
    int sender_id;
} Message;

// stany procesu
typedef enum
{
    REST,
    WAIT,
    INSECTION
} State;

int rank, size;
// Lamport clock z synchronizacją między wątkami
int lamport_clock = 0;
std::mutex clock_mutex;
void increment_clock(int received);
int read_clock();

// Licznik odebranych ACK
int ack_counter = 0;
std::mutex ack_mutex;
void update_ack(int new_ack);
int get_ack();

// Tablica do śledzenia timestampów wiadomości REQ
int *timestamp_of_request;
std::mutex timestamp_mutex;
void init_timestamp_array();
void update_timestamp_array(int index, int value);
int get_timestamp_array(int index);

// Miasto, o które staram się
int request_city = -1;
std::mutex request_mutex;
void update_request_city(int city_id);
int get_request_city();

// Aktualny stan procesu
State current_state = REST;
std::mutex state_mutex;
void update_state(State new_state);
State get_state();

// Tablica do śledzenia odebranych ACK
bool *ack_received_from;
std::mutex ack_received_mutex;
void init_ack_received_array();
void update_ack_received(int index, bool value);
bool get_ack_received(int index);

// Tablica do śledzenia, komu będzie trzeba wysłać ACK po wyjściu z sekcji krytycznej
bool *deferred_replies;
std::mutex deferred_replies_mutex;
void init_deferred_replies_array();
void update_deferred_replies(int index, bool value);
bool get_deferred_replies(int index);

// Wysyła wiadomość do procesu docelowego, nie dbając o synchronizację zegara Lamporta
void send_message(Message msg, int dest)
{
    MPI_Send(&msg, sizeof(Message), MPI_BYTE, dest, TAG, MPI_COMM_WORLD);
    printf("[%d] [t%d] Wyslano %s do %d dla miasta %d\n", rank, lamport_clock,
           msg.type == REQ ? "REQ" : "ACK", dest, msg.city_id);
}

// porównanie priorytetów wiadomości
int has_higher_priority(Message a, Message b)
{
    if (a.timestamp < b.timestamp)
        return 1;
    if (a.timestamp == b.timestamp && a.sender_id < b.sender_id)
        return 1;
    return 0;
}

void handle_message(Message *msg)
{
    increment_clock(msg->timestamp);

    if (msg->type == REQ)
    {
        printf("[%d] [t%d] Otrzymano REQ od %d dla miasta %d\n", rank, read_clock(), msg->sender_id, msg->city_id);

        bool send_ack = false;

        // decyzja o wysłaniu ACK
        if (get_state() == REST)
        {
            send_ack = true;
        }
        else if (get_state() == WAIT && get_request_city() != msg->city_id)
        {
            send_ack = true;
        }
        else if (get_state() == WAIT)
        {
            Message my_req = {REQ, get_request_city(), get_timestamp_array(msg->sender_id), rank};
            send_ack = has_higher_priority(*msg, my_req);
        }
        else if (get_state() == INSECTION && get_request_city() != msg->city_id)
        {
            send_ack = true;
        }

        if (send_ack)
        {
            increment_clock(0);
            Message ack = {ACK, msg->city_id, read_clock(), rank};
            send_message(ack, msg->sender_id);
        }
        else
        {
            update_deferred_replies(msg->sender_id, true);
        }
    }
    else if (msg->type == ACK)
    {
        printf("[%d] [t%d] Otrzymano ACK od %d dla miasta %d\n", rank, read_clock(), msg->sender_id, msg->city_id);
        if (!get_ack_received(msg->sender_id))
        {
            update_ack_received(msg->sender_id, true);
            update_ack(get_ack() + 1);
        }
    }
}

void receive_loop()
{
    while (1)
    {
        Message msg;
        MPI_Recv(&msg, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        handle_message(&msg);
    }
}

void enter_city(int city_id)
{
    update_ack(0);
    init_timestamp_array();
    update_request_city(city_id);
    update_state(WAIT);
    init_ack_received_array();
    init_deferred_replies_array();

    increment_clock(0);
    printf("[%d] [t%d] Rozpoczynam staranie o miasto %d\n", rank, read_clock(), city_id);

    // Wysłanie żądań do wszystkich innych procesów
    for (int i = 0; i < size; i++)
    {
        if (i != rank)
        {
            increment_clock(0);
            update_timestamp_array(i, read_clock());
            Message req = {REQ, city_id, read_clock(), rank};
            send_message(req, i);
        }
    }

    // Czekanie na ACK od wszystkich innych procesów
    while (get_ack() < size - 1)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    update_state(INSECTION);
    printf("[%d] [t%d] Wchodze do miasta %d\n", rank, read_clock(), city_id);
    std::this_thread::sleep_for(std::chrono::seconds(rand() % MAX_TIME_IN_CITY)); // Symulacja przebywania w mieście

    printf("[%d] [t%d] Wychodze z miasta %d\n", rank, read_clock(), city_id);

    // wysłanie zaległych ACK
    for (int i = 0; i < size; i++)
    {
        if (get_deferred_replies(i))
        {
            increment_clock(0);
            Message ack = {ACK, city_id, read_clock(), rank};
            send_message(ack, i);
            update_deferred_replies(i, false);
        }
    }

    update_state(REST);
    update_request_city(-1);
}

void live_loop()
{
    while (1)
    {
        printf("[%d] [t%d] Spie\n", rank, read_clock());
        std::this_thread::sleep_for(std::chrono::seconds(rand() % MAX_TIME_DOING_NOTHING));
        int city = rand() % NUM_CITIES;
        enter_city(city);
    }
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    init_ack_received_array();
    init_deferred_replies_array();
    init_timestamp_array(); // ← DODAJ TO

    if (rank == 0)
        printf("Rozpoczynam dzialanie %d procesow\n", size);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    srand(time(NULL) + rank);

    std::thread receiver_thread(receive_loop);
    std::thread live_thread(live_loop);

    receiver_thread.join();
    live_thread.join(); // ← bez join() program kończy się natychmiast, zanim wątki zrobią cokolwiek

    MPI_Finalize();
    return 0;
}

//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
//
void increment_clock(int received)
{
    std::lock_guard<std::mutex> lock(clock_mutex);
    lamport_clock = (lamport_clock > received ? lamport_clock : received) + 1;
}

int read_clock()
{
    std::lock_guard<std::mutex> lock(clock_mutex);
    return lamport_clock;
}
void update_ack(int new_ack)
{
    std::lock_guard<std::mutex> lock(ack_mutex);
    ack_counter = new_ack;
}
int get_ack()
{
    std::lock_guard<std::mutex> lock(ack_mutex);
    return ack_counter;
}
void init_timestamp_array()
{
    timestamp_of_request = new int[size];
    for (int i = 0; i < size; i++)
    {
        timestamp_of_request[i] = 0;
    }
}

void update_timestamp_array(int index, int value)
{
    std::lock_guard<std::mutex> lock(timestamp_mutex);
    timestamp_of_request[index] = value;
}

int get_timestamp_array(int index)
{
    std::lock_guard<std::mutex> lock(timestamp_mutex);
    return timestamp_of_request[index];
}

void update_request_city(int city_id)
{
    std::lock_guard<std::mutex> lock(request_mutex);
    request_city = city_id;
}

int get_request_city()
{
    std::lock_guard<std::mutex> lock(request_mutex);
    return request_city;
}

void update_state(State new_state)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    current_state = new_state;
}

State get_state()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return current_state;
}

void init_ack_received_array()
{
    ack_received_from = new bool[size];
    for (int i = 0; i < size; i++)
    {
        ack_received_from[i] = false;
    }
}

void update_ack_received(int index, bool value)
{
    std::lock_guard<std::mutex> lock(ack_received_mutex);
    ack_received_from[index] = value;
}

bool get_ack_received(int index)
{
    std::lock_guard<std::mutex> lock(ack_received_mutex);
    return ack_received_from[index];
}

void init_deferred_replies_array()
{
    deferred_replies = new bool[size];
    for (int i = 0; i < size; i++)
    {
        deferred_replies[i] = false;
    }
}

void update_deferred_replies(int index, bool value)
{
    std::lock_guard<std::mutex> lock(deferred_replies_mutex);
    deferred_replies[index] = value;
}

bool get_deferred_replies(int index)
{
    std::lock_guard<std::mutex> lock(deferred_replies_mutex);
    return deferred_replies[index];
}