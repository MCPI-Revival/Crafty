#include <algorithm>
#include <queue>
#include <pthread.h>

#include <symbols/minecraft.h>
#include <mods/chat/chat.h>
#include <mods/misc/misc.h>

#include <libreborn/util/exec.h>
#include <libreborn/util/util.h>
#include <libreborn/util/string.h>
#include <libreborn/log.h>

// Main Lock
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Handle Messages
static std::queue<std::string> queue;
static void tick(Minecraft *minecraft) {
    if (!queue.empty()) {
        NetEventCallback *network_handler = minecraft->network_handler;
        RakNetInstance *rak_net_instance = minecraft->rak_net_instance;
        if (network_handler && rak_net_instance && rak_net_instance->isServer()) {
            pthread_mutex_lock(&lock);
            while (!queue.empty()) {
                ServerSideNetworkHandler *server = (ServerSideNetworkHandler *) network_handler;
                server->displayGameMessage(std::string("<Crafty> ") + queue.front());
                queue.pop();
            }
            pthread_mutex_unlock(&lock);
        }
    }
}

// String Functions
static void ltrim(std::string &s) {
    s.erase(s.begin(), std::ranges::find_if(s, [](const unsigned char ch) {
        return !std::isspace(ch);
    }));
}
static void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](const unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}
void replace_all(std::string &s, const std::string &toReplace, const std::string &replaceWith) {
    std::string buf;
    std::size_t pos = 0;
    std::size_t prevPos;
    buf.reserve(s.size());
    while (true) {
        prevPos = pos;
        pos = s.find(toReplace, pos);
        if (pos == std::string::npos) {
            break;
        }
        buf.append(s, prevPos, pos - prevPos);
        buf += replaceWith;
        pos += toReplace.size();
    }
    buf.append(s, prevPos, s.size() - prevPos);
    s.swap(buf);
}

// Thread
static volatile bool running = true;
static pthread_t thread;
static std::queue<std::string> to_process;
static void *ai_thread(void *) {
    while (running) {
        if (!to_process.empty()) {
            pthread_mutex_lock(&lock);
            while (!to_process.empty()) {
                std::string message = to_process.front();
                to_process.pop();
                int status = 0;
                std::string prompt = "You are a digital assistant named Crafty, please answer the following chat message in a humorous manner using excessive Minecraft references while limiting your response to 1-2 sentences without using emojis: " + message;
                const char *const command[] = {"ollama", "run", "gemma3", prompt.c_str(), "--verbose", "--nowordwrap", nullptr};
                const std::vector<unsigned char> *output = run_command(command, &status);
                std::string output_str = (const char *) output->data();
                delete output;
                if (is_exit_status_success(status)) {
                    rtrim(output_str);
                    ltrim(output_str);
                    replace_all(output_str, "’", "\'");
                    replace_all(output_str, "–", "-");
                    output_str = to_cp437(output_str);
                    queue.push(output_str);
                } else {
                    WARN("AI Error: %s", output_str.c_str());
                }
            }
            pthread_mutex_unlock(&lock);
        }
    }
    return nullptr;
}

// Handle Chat Message
HOOK(chat_send_message_to_clients, void, (ServerSideNetworkHandler *server_side_network_handler, const Player *sender, const char *message)) {
    // Call Original Method
    real_chat_send_message_to_clients()(server_side_network_handler, sender, message);
    // Queue
    pthread_mutex_lock(&lock);
    to_process.emplace(message);
    pthread_mutex_unlock(&lock);
}

// Handle Exit
HOOK(compat_rquest_exit, void, ()) {
    // Call Original Method
    real_compat_rquest_exit()();
    // Stop Thread
    running = false;
}

// Update Version
HOOK(_Z18reborn_get_versionv, const char *, ()) {
    static std::string out;
    if (out.empty()) {
        out = real__Z18reborn_get_versionv()();
        out += " (+AI)";
    }
    return out.c_str();
}

// Init
__attribute__((constructor)) static void init_custom_block() {
    misc_run_on_tick(tick);
    misc_run_on_init([](Minecraft *) {
        pthread_create(&thread,  nullptr, ai_thread, nullptr);
    });
}