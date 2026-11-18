/*
 * MSPC (MOLOTOK SYSTEM Peripheral System)
 * Copyright (C) 2026 Molotok
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but annoying to plagiarists, WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 */

#include <stddef.h>
#include <stdint.h>
#include "drivers/driver.h"
#include "drivers/mspc_gl.h"

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define MAX_LINE 128
#define FS_NODE_COUNT 16
#define FS_NAME_SIZE 24
#define FS_PATH_SIZE 128
#define FS_DATA_SIZE 1024
#define HISTORY_COUNT 8
#define TERM_HISTORY_ROWS 32
#define TERM_VIEW_ROWS 12
#define TERM_VIEW_COLS 36
#define ACCOUNT_COUNT 8
#define ACCOUNT_NAME_SIZE 16
#define ACCESS_READ 4
#define ACCESS_WRITE 2
#define ACCESS_EXEC 1
#define PROGRAM_VARIABLE_COUNT 8
#define PROGRAM_MAX_LINES 64
#define DOWNLOAD_BUFFER_SIZE 8192

static uint8_t color = 0x0f;
static char input_line[MAX_LINE];
static size_t input_length;
static char editor_buffer[FS_DATA_SIZE];
static uint8_t download_buffer[DOWNLOAD_BUFFER_SIZE];
static char terminal_output[TERM_HISTORY_ROWS][TERM_VIEW_COLS + 1];
static size_t terminal_rows;
static size_t terminal_column;
static size_t editor_length;
static size_t editor_position;
static size_t editor_scroll_line;
static int editor_file = -1;
static int editor_dirty;
static int active_window;
static int explorer_directory;
static size_t explorer_selection;
static size_t explorer_scroll;

typedef struct {
    uint8_t used;
    uint8_t is_directory;
    uint8_t owner;
    uint16_t mode;
    int parent;
    size_t size;
    char name[FS_NAME_SIZE];
    char data[FS_DATA_SIZE];
} fs_node_t;

typedef struct {
    uint8_t used;
    uint8_t is_root;
    uint32_t password_hash;
    char name[ACCOUNT_NAME_SIZE];
} account_t;

typedef struct {
    const char *manifest_name;
    int (*initialize)(void);
    uint8_t loaded;
} driver_entry_t;

typedef struct {
    uint8_t used;
    char name[ACCOUNT_NAME_SIZE];
    int32_t value;
} program_variable_t;

static fs_node_t fs_nodes[FS_NODE_COUNT] = {
    [0] = { .used = 1, .is_directory = 1, .owner = 0, .mode = 0755, .parent = -1, .name = "/" }
};
static int current_directory;
static account_t accounts[ACCOUNT_COUNT];
static uint8_t current_account;
static size_t account_count;
static driver_entry_t driver_registry[] = {
    { "ps2-keyboard.drv", duck_ps2_keyboard_init, 0 },
    { "vga-text.drv", duck_vga_text_init, 0 },
    { "pcnet.drv", duck_pcnet_init, 0 }
};
static char command_history[HISTORY_COUNT][MAX_LINE];
static size_t history_count;
static size_t history_next;

static const char *command_names[] = {
    "help", "clear", "echo", "about", "ver", "mem", "cpu", "uname", "pwd",
    "ls", "cd", "mkdir", "touch", "cat", "write", "rm", "cp", "mv", "tree",
    "df", "reboot", "whoami", "env", "calc", "hex", "dec", "history", "etxt",
    "users", "useradd", "passwd", "su", "root", "chmod", "drivers", "driver", "run",
    "net", "ifconfig", "dns", "ping", "download", "gltest", "3dtest", "gfx"
};

extern const uint8_t duck_driver_pack_start[];
extern const uint8_t duck_driver_pack_end[];

static void screen_put_char(char character) {
    duck_vga_write(character, color);
}

static void put_char(char character) {
    screen_put_char(character);
    if (!terminal_rows) terminal_rows = 1;
    size_t row = terminal_rows - 1;
    if (character == '\n') {
        terminal_column = 0;
        if (terminal_rows < TERM_HISTORY_ROWS) {
            terminal_rows++;
        } else {
            for (size_t i = 1; i < TERM_HISTORY_ROWS; i++) {
                for (size_t j = 0; j <= TERM_VIEW_COLS; j++) terminal_output[i - 1][j] = terminal_output[i][j];
            }
        }
        for (size_t j = 0; j <= TERM_VIEW_COLS; j++) terminal_output[terminal_rows - 1][j] = '\0';
    } else if (character == '\r') {
        terminal_column = 0;
    } else if (character == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
            terminal_output[row][terminal_column] = '\0';
        }
    } else {
        if (terminal_column < TERM_VIEW_COLS) {
            terminal_output[row][terminal_column++] = character;
            terminal_output[row][terminal_column] = '\0';
        }
    }
}

static void print(const char *text) {
    while (*text) {
        put_char(*text++);
    }
}

static void clear_screen(void) {
    duck_vga_clear(color);
    for (size_t row = 0; row < TERM_HISTORY_ROWS; row++) {
        for (size_t column = 0; column <= TERM_VIEW_COLS; column++) {
            terminal_output[row][column] = '\0';
        }
    }
    terminal_rows = 1;
    terminal_column = 0;
}

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static char scancode_to_ascii(uint8_t code) {
    static const char keys[128] = {
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
        [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9', [0x0b] = '0',
        [0x0c] = '-', [0x0d] = '=', [0x0e] = '\b', [0x0f] = '\t',
        [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
        [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
        [0x1a] = '[', [0x1b] = ']', [0x1c] = '\n', [0x1e] = 'a', [0x1f] = 's',
        [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j',
        [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
        [0x2b] = '\\', [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v',
        [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
        [0x35] = '/', [0x39] = ' '
    };
    return code < 128 ? keys[code] : 0;
}

static int equal(const char *left, const char *right) {
    while (*left && *right && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

static size_t string_length(const char *text);
static void string_copy(char *destination, const char *source, size_t capacity);
static int create_node(const char *path, uint8_t is_directory);
static int resolve_path(const char *path);

static int manifest_enabled(const uint8_t *data, size_t length) {
    static const char setting[] = "enabled=1";
    for (size_t i = 0; i + sizeof(setting) - 1 <= length; i++) {
        size_t j = 0;
        while (j < sizeof(setting) - 1 && data[i + j] == (uint8_t)setting[j]) j++;
        if (j == sizeof(setting) - 1) return 1;
    }
    return 0;
}

static int activate_driver(const char *name, const uint8_t *data, size_t size) {
    if (!manifest_enabled(data, size)) return -1;
    for (size_t i = 0; i < sizeof(driver_registry) / sizeof(driver_registry[0]); i++) {
        if (!equal(name, driver_registry[i].manifest_name)) continue;
        if (driver_registry[i].loaded) return 2;
        if (!driver_registry[i].initialize()) return 0;
        driver_registry[i].loaded = 1;
        return 1;
    }
    return -1;
}

static int walk_driver_pack(int copy_to_ramfs) {
    const uint8_t *cursor = duck_driver_pack_start;
    const uint8_t *end = duck_driver_pack_end;
    static const uint8_t magic[8] = { 'M', 'S', 'P', 'C', 'D', 'R', 'V', '1' };
    if (end < cursor || (size_t)(end - cursor) < 10) return 0;
    for (size_t i = 0; i < sizeof(magic); i++) if (cursor[i] != magic[i]) return 0;
    cursor += sizeof(magic);
    uint16_t count = (uint16_t)cursor[0] | (uint16_t)cursor[1] << 8;
    cursor += 2;

    if (copy_to_ramfs) {
        int directory = resolve_path("/drivers");
        if (directory < 0) directory = create_node("/drivers", 1);
        if (directory < 0 || !fs_nodes[directory].is_directory) return 0;
        fs_nodes[directory].owner = 0;
        fs_nodes[directory].mode = 0755;
    }

    for (uint16_t item = 0; item < count; item++) {
        if ((size_t)(end - cursor) < 3) return 0;
        uint8_t name_length = cursor[0];
        uint16_t data_length = (uint16_t)cursor[1] | (uint16_t)cursor[2] << 8;
        cursor += 3;
        if (!name_length || name_length >= FS_NAME_SIZE || data_length >= FS_DATA_SIZE ||
            (size_t)(end - cursor) < (size_t)name_length + data_length) return 0;
        char name[FS_NAME_SIZE];
        for (uint8_t i = 0; i < name_length; i++) name[i] = (char)cursor[i];
        name[name_length] = '\0';
        const uint8_t *data = cursor + name_length;
        activate_driver(name, data, data_length);

        if (copy_to_ramfs) {
            char path[FS_PATH_SIZE] = "/drivers/";
            string_copy(path + 9, name, sizeof(path) - 9);
            int file = resolve_path(path);
            if (file < 0) file = create_node(path, 0);
            if (file < 0 || fs_nodes[file].is_directory) return 0;
            for (uint16_t i = 0; i < data_length; i++) fs_nodes[file].data[i] = (char)data[i];
            fs_nodes[file].data[data_length] = '\0';
            fs_nodes[file].size = data_length;
            fs_nodes[file].owner = 0;
            fs_nodes[file].mode = 0644;
        }
        cursor = data + data_length;
    }
    return cursor == end;
}

static int find_account(const char *name) {
    for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
        if (accounts[i].used && equal(accounts[i].name, name)) return (int)i;
    }
    return -1;
}

static uint32_t hash_password(const char *password) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; password[i]; i++) {
        hash ^= (uint8_t)password[i];
        hash *= 16777619u;
    }
    return hash;
}

static int current_is_root(void) {
    return accounts[current_account].used && accounts[current_account].is_root;
}

static int has_access(int node, uint16_t access) {
    if (node < 0 || node >= FS_NODE_COUNT || !fs_nodes[node].used) return 0;
    if (current_is_root()) return 1;
    uint16_t granted = fs_nodes[node].owner == current_account ? (fs_nodes[node].mode >> 6) : fs_nodes[node].mode;
    return (granted & access) == access;
}

static int valid_account_name(const char *name) {
    size_t length = string_length(name);
    if (!length || length >= ACCOUNT_NAME_SIZE) return 0;
    for (size_t i = 0; i < length; i++) {
        char character = name[i];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_')) return 0;
    }
    return 1;
}

static void read_keyboard_line(char *output, size_t capacity, int hidden) {
    size_t length = 0;
    for (;;) {
        int key = duck_ps2_read_char();
        if (key < 0) continue;
        char character = (char)key;
        if (character == '\n') {
            output[length] = '\0';
            put_char('\n');
            return;
        }
        if (character == '\b' && length) {
            output[--length] = '\0';
            if (hidden) put_char('\b');
            else put_char('\b');
        } else if (character >= ' ' && length + 1 < capacity) {
            output[length++] = character;
            if (hidden) put_char('*');
            else put_char(character);
        }
    }
}

static void create_first_account(void) {
    char name[ACCOUNT_NAME_SIZE];
    char password[32];
    print("MSPC first start: create the initial root account.\n");
    for (;;) {
        print("Root name: ");
        read_keyboard_line(name, sizeof(name), 0);
        if (valid_account_name(name)) break;
        print("Use 1-15 lowercase letters, digits, or underscore.\n");
    }
    do {
        print("Root password: ");
        read_keyboard_line(password, sizeof(password), 1);
        if (!password[0]) print("Password cannot be empty.\n");
    } while (!password[0]);

    accounts[0].used = 1;
    accounts[0].is_root = 1;
    accounts[0].password_hash = hash_password(password);
    string_copy(accounts[0].name, name, sizeof(accounts[0].name));
    account_count = 1;
    current_account = 0;

    int system_directory = create_node("/system", 1);
    if (system_directory >= 0) {
        fs_nodes[system_directory].owner = 0;
        fs_nodes[system_directory].mode = 0755;
        int config = create_node("/system/boot.conf", 0);
        if (config >= 0) {
            fs_nodes[config].owner = 0;
            fs_nodes[config].mode = 0600;
            string_copy(fs_nodes[config].data, "MSPC command-line system\n", FS_DATA_SIZE);
            fs_nodes[config].size = string_length(fs_nodes[config].data);
        }
    }
}

static size_t string_length(const char *text) {
    size_t length = 0;
    while (text[length]) length++;
    return length;
}

static void string_copy(char *destination, const char *source, size_t capacity) {
    size_t i = 0;
    while (source[i] && i + 1 < capacity) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = '\0';
}

static void print_unsigned(uint32_t value, uint32_t base) {
    char digits[32];
    size_t count = 0;
    do {
        uint32_t digit = value % base;
        digits[count++] = digit < 10 ? (char)('0' + digit) : (char)('a' + digit - 10);
        value /= base;
    } while (value && count < sizeof(digits));
    while (count) put_char(digits[--count]);
}

static int find_child(int parent, const char *name) {
    for (size_t i = 1; i < FS_NODE_COUNT; i++) {
        if (fs_nodes[i].used && fs_nodes[i].parent == parent && equal(fs_nodes[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static int resolve_path(const char *path) {
    int node = path[0] == '/' ? 0 : current_directory;
    size_t position = 0;
    while (path[position]) {
        while (path[position] == '/') position++;
        if (!path[position]) break;
        char component[FS_NAME_SIZE];
        size_t length = 0;
        while (path[position] && path[position] != '/') {
            if (length + 1 >= sizeof(component)) return -1;
            component[length++] = path[position++];
        }
        component[length] = '\0';
        if (equal(component, ".")) continue;
        if (equal(component, "..")) {
            if (node != 0) node = fs_nodes[node].parent;
            continue;
        }
        if (!has_access(node, ACCESS_EXEC)) return -1;
        node = find_child(node, component);
        if (node < 0) return -1;
    }
    return node;
}

static int create_node(const char *path, uint8_t is_directory) {
    char parent_path[FS_PATH_SIZE];
    char name[FS_NAME_SIZE];
    size_t length = string_length(path);
    if (!length || length >= sizeof(parent_path)) return -1;
    while (length && path[length - 1] == '/') length--;
    if (!length) return -1;

    size_t name_start = length;
    while (name_start && path[name_start - 1] != '/') name_start--;
    size_t name_length = length - name_start;
    if (!name_length || name_length >= sizeof(name)) return -1;
    for (size_t i = 0; i < name_length; i++) name[i] = path[name_start + i];
    name[name_length] = '\0';
    if (equal(name, ".") || equal(name, "..")) return -1;

    if (!name_start) {
        string_copy(parent_path, ".", sizeof(parent_path));
    } else if (name_start == 1 && path[0] == '/') {
        string_copy(parent_path, "/", sizeof(parent_path));
    } else {
        size_t parent_length = name_start - 1;
        for (size_t i = 0; i < parent_length; i++) parent_path[i] = path[i];
        parent_path[parent_length] = '\0';
    }
    int parent = resolve_path(parent_path);
    if (parent < 0 || !fs_nodes[parent].is_directory ||
        !has_access(parent, ACCESS_WRITE | ACCESS_EXEC) ||
        find_child(parent, name) >= 0) return -1;

    for (size_t i = 1; i < FS_NODE_COUNT; i++) {
        if (!fs_nodes[i].used) {
            fs_nodes[i].used = 1;
            fs_nodes[i].is_directory = is_directory;
            fs_nodes[i].owner = current_account;
            fs_nodes[i].mode = is_directory ? 0755 : 0644;
            fs_nodes[i].parent = parent;
            fs_nodes[i].size = 0;
            string_copy(fs_nodes[i].name, name, sizeof(fs_nodes[i].name));
            fs_nodes[i].data[0] = '\0';
            return (int)i;
        }
    }
    return -1;
}

static int resolve_or_report(const char *path) {
    int node = resolve_path(path);
    if (node < 0) print("Path not found.\n");
    return node;
}

static void print_working_directory(void) {
    int chain[FS_NODE_COUNT];
    size_t count = 0;
    int node = current_directory;
    while (node > 0 && count < FS_NODE_COUNT) {
        chain[count++] = node;
        node = fs_nodes[node].parent;
    }
    put_char('/');
    while (count) {
        print(fs_nodes[chain[--count]].name);
        if (count) put_char('/');
    }
    put_char('\n');
}

static void list_directory(int directory) {
    int found = 0;
    for (size_t i = 1; i < FS_NODE_COUNT; i++) {
        if (!fs_nodes[i].used || fs_nodes[i].parent != directory) continue;
        print(fs_nodes[i].name);
        if (fs_nodes[i].is_directory) put_char('/');
        else {
            print("  ");
            print_unsigned((uint32_t)fs_nodes[i].size, 10);
            print(" B");
        }
        put_char('\n');
        found = 1;
    }
    if (!found) print("(empty)\n");
}

static void print_tree(int directory, size_t depth) {
    if (!has_access(directory, ACCESS_READ | ACCESS_EXEC)) {
        for (size_t level = 0; level < depth; level++) print("  ");
        print("Permission denied.\n");
        return;
    }
    for (size_t i = 1; i < FS_NODE_COUNT; i++) {
        if (!fs_nodes[i].used || fs_nodes[i].parent != directory) continue;
        for (size_t level = 0; level < depth; level++) print("  ");
        print(fs_nodes[i].name);
        if (fs_nodes[i].is_directory) {
            print("/\n");
            print_tree((int)i, depth + 1);
        } else {
            put_char('\n');
        }
    }
}

static void remember_command(const char *line) {
    if (!*line) return;
    string_copy(command_history[history_next], line, MAX_LINE);
    history_next = (history_next + 1) % HISTORY_COUNT;
    if (history_count < HISTORY_COUNT) history_count++;
}

static char *next_token(char **cursor) {
    while (**cursor == ' ') (*cursor)++;
    char *token = *cursor;
    while (**cursor && **cursor != ' ') (*cursor)++;
    if (**cursor) *(*cursor)++ = '\0';
    return token;
}

static uint32_t parse_number(const char *text, uint32_t base, int *valid) {
    uint32_t value = 0;
    *valid = 0;
    if (!*text) return 0;
    for (size_t i = 0; text[i]; i++) {
        uint32_t digit;
        if (text[i] >= '0' && text[i] <= '9') digit = (uint32_t)(text[i] - '0');
        else if (text[i] >= 'a' && text[i] <= 'f') digit = (uint32_t)(text[i] - 'a' + 10);
        else if (text[i] >= 'A' && text[i] <= 'F') digit = (uint32_t)(text[i] - 'A' + 10);
        else return 0;
        if (digit >= base || value > (0xffffffffu - digit) / base) return 0;
        value = value * base + digit;
    }
    *valid = 1;
    return value;
}

static int parse_signed_number(const char *text, int32_t *value) {
    int negative = text[0] == '-';
    const char *digits = negative ? text + 1 : text;
    int valid;
    uint32_t magnitude = parse_number(digits, 10, &valid);
    if (!valid || magnitude > 2147483648u || (!negative && magnitude > 2147483647u)) return 0;
    if (negative && magnitude == 2147483648u) *value = (-2147483647 - 1);
    else *value = negative ? -(int32_t)magnitude : (int32_t)magnitude;
    return 1;
}

static int program_variable(program_variable_t *variables, const char *name, int create) {
    int free_slot = -1;
    for (size_t i = 0; i < PROGRAM_VARIABLE_COUNT; i++) {
        if (variables[i].used && equal(variables[i].name, name)) return (int)i;
        if (!variables[i].used && free_slot < 0) free_slot = (int)i;
    }
    if (!create || free_slot < 0 || !valid_account_name(name)) return -1;
    variables[free_slot].used = 1;
    variables[free_slot].value = 0;
    string_copy(variables[free_slot].name, name, sizeof(variables[free_slot].name));
    return free_slot;
}

static void print_signed(int32_t value) {
    if (value < 0) {
        put_char('-');
        print_unsigned(0u - (uint32_t)value, 10);
    } else {
        print_unsigned((uint32_t)value, 10);
    }
}

static int program_print_text(const char *text, program_variable_t *variables) {
    while (*text) {
        if (*text != '$') {
            put_char(*text++);
            continue;
        }
        text++;
        char name[ACCOUNT_NAME_SIZE];
        size_t length = 0;
        while (((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z') ||
                (*text >= '0' && *text <= '9') || *text == '_') && length + 1 < sizeof(name)) {
            name[length++] = *text++;
        }
        if (!length) {
            put_char('$');
            continue;
        }
        name[length] = '\0';
        int variable = program_variable(variables, name, 0);
        if (variable < 0) {
            print("\nUnknown variable: "); print(name); put_char('\n');
            return 0;
        }
        print_signed(variables[variable].value);
    }
    return 1;
}

static int execute_program(int file_index) {
    if (file_index < 0 || fs_nodes[file_index].is_directory) {
        print("Not a program file.\n");
        return 0;
    }
    if (!has_access(file_index, ACCESS_READ)) {
        print("Permission denied.\n");
        return 0;
    }

    uint16_t line_starts[PROGRAM_MAX_LINES];
    uint16_t line_lengths[PROGRAM_MAX_LINES];
    size_t line_count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= fs_nodes[file_index].size; i++) {
        if (i != fs_nodes[file_index].size && fs_nodes[file_index].data[i] != '\n') continue;
        if (line_count == PROGRAM_MAX_LINES || i - start >= MAX_LINE) {
            print("Program exceeds the line or line-length limit.\n");
            return 0;
        }
        line_starts[line_count] = (uint16_t)start;
        line_lengths[line_count] = (uint16_t)(i - start);
        line_count++;
        start = i + 1;
    }

    program_variable_t variables[PROGRAM_VARIABLE_COUNT] = { 0 };
    size_t pc = 0;
    size_t steps = 0;
    while (pc < line_count) {
        if (++steps > 1000) {
            print("Program stopped: instruction limit reached.\n");
            return 0;
        }
        char line[MAX_LINE];
        size_t length = line_lengths[pc];
        for (size_t i = 0; i < length; i++) line[i] = fs_nodes[file_index].data[line_starts[pc] + i];
        line[length] = '\0';
        char *cursor = line;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (!*cursor || *cursor == '#') {
            pc++;
            continue;
        }

        char *instruction = next_token(&cursor);
        if (equal(instruction, "end")) return 1;
        if (equal(instruction, "print")) {
            while (*cursor == ' ') cursor++;
            size_t text_length = string_length(cursor);
            if (text_length >= 2 && cursor[0] == '"' && cursor[text_length - 1] == '"') {
                cursor[text_length - 1] = '\0';
                cursor++;
            }
            if (!program_print_text(cursor, variables)) return 0;
            put_char('\n');
        } else if (equal(instruction, "clear")) {
            clear_screen();
        } else if (equal(instruction, "let")) {
            char *name = next_token(&cursor);
            char *number = next_token(&cursor);
            int32_t value;
            int variable = program_variable(variables, name, 1);
            if (variable < 0 || !parse_signed_number(number, &value)) {
                print("Program error at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                return 0;
            }
            variables[variable].value = value;
        } else if (equal(instruction, "show")) {
            char *name = next_token(&cursor);
            int variable = program_variable(variables, name, 0);
            if (variable < 0) {
                print("Unknown variable at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                return 0;
            }
            print_signed(variables[variable].value);
            put_char('\n');
        } else if (equal(instruction, "add") || equal(instruction, "sub") ||
                   equal(instruction, "mul") || equal(instruction, "div")) {
            char *name = next_token(&cursor);
            char *number = next_token(&cursor);
            int32_t value;
            int variable = program_variable(variables, name, 0);
            if (variable < 0 || !parse_signed_number(number, &value) ||
                (equal(instruction, "div") && value == 0)) {
                print("Invalid arithmetic instruction at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                return 0;
            }
            if (equal(instruction, "add")) variables[variable].value += value;
            else if (equal(instruction, "sub")) variables[variable].value -= value;
            else if (equal(instruction, "mul")) variables[variable].value *= value;
            else variables[variable].value /= value;
        } else if (equal(instruction, "input")) {
            char *name = next_token(&cursor);
            char value_text[MAX_LINE];
            int32_t value;
            int variable = program_variable(variables, name, 0);
            print("Input "); print(name); print(": ");
            read_keyboard_line(value_text, sizeof(value_text), 0);
            if (variable < 0 || !parse_signed_number(value_text, &value)) {
                print("Invalid input at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                return 0;
            }
            variables[variable].value = value;
        } else if (equal(instruction, "cat")) {
            char *path = next_token(&cursor);
            int file = resolve_path(path);
            if (file < 0 || fs_nodes[file].is_directory || !has_access(file, ACCESS_READ)) {
                print("Cannot read program file path at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                return 0;
            }
            for (size_t i = 0; i < fs_nodes[file].size; i++) put_char(fs_nodes[file].data[i]);
            put_char('\n');
        } else if (equal(instruction, "write") || equal(instruction, "append")) {
            int append = equal(instruction, "append");
            char *path = next_token(&cursor);
            while (*cursor == ' ') cursor++;
            int file = resolve_path(path);
            if (file < 0) file = create_node(path, 0);
            if (file < 0 || fs_nodes[file].is_directory || !has_access(file, ACCESS_WRITE)) {
                print("Cannot write program file path at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                return 0;
            }
            size_t offset = append ? fs_nodes[file].size : 0;
            size_t text_size = string_length(cursor);
            if (offset + text_size >= FS_DATA_SIZE) {
                print("Program write exceeds RAMFS file limit.\n");
                return 0;
            }
            for (size_t i = 0; i < text_size; i++) fs_nodes[file].data[offset + i] = cursor[i];
            fs_nodes[file].data[offset + text_size] = '\0';
            fs_nodes[file].size = offset + text_size;
        } else if (equal(instruction, "ping")) {
            char *host = next_token(&cursor);
            int result = duck_network_ping(host);
            print(result == 1 ? "ping: reply\n" : "ping: failed\n");
        } else if (equal(instruction, "dns")) {
            char *host = next_token(&cursor);
            uint8_t address[4];
            if (!duck_network_resolve(host, address)) {
                print("dns: lookup failed\n");
            } else {
                for (size_t i = 0; i < 4; i++) {
                    if (i) put_char('.');
                    print_unsigned(address[i], 10);
                }
                put_char('\n');
            }
        } else if (equal(instruction, "goto")) {
            char *target_text = next_token(&cursor);
            int valid;
            uint32_t target = parse_number(target_text, 10, &valid);
            if (!valid || target == 0 || target > line_count) {
                print("Invalid jump at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                return 0;
            }
            pc = target - 1;
            continue;
        } else if (equal(instruction, "if")) {
            char *name = next_token(&cursor);
            char *operation = next_token(&cursor);
            char *number = next_token(&cursor);
            char *jump_word = next_token(&cursor);
            char *target_text = next_token(&cursor);
            int variable = program_variable(variables, name, 0);
            int32_t value;
            int valid = variable >= 0 && parse_signed_number(number, &value);
            int condition = 0;
            if (!valid || !equal(jump_word, "goto")) {
                print("Invalid condition at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                return 0;
            }
            if (equal(operation, "==")) condition = variables[variable].value == value;
            else if (equal(operation, "!=")) condition = variables[variable].value != value;
            else if (equal(operation, "<")) condition = variables[variable].value < value;
            else if (equal(operation, ">")) condition = variables[variable].value > value;
            else {
                print("Unknown comparison at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                return 0;
            }
            if (condition) {
                int target_valid;
                uint32_t target = parse_number(target_text, 10, &target_valid);
                if (!target_valid || target == 0 || target > line_count) {
                    print("Invalid jump at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
                    return 0;
                }
                pc = target - 1;
                continue;
            }
        } else {
            print("Unknown instruction at line "); print_unsigned((uint32_t)pc + 1, 10); put_char('\n');
            return 0;
        }
        pc++;
    }
    return 1;
}

static void open_editor_file(int file_index) {
    if (file_index < 0 || fs_nodes[file_index].is_directory) return;
    editor_file = file_index;
    editor_length = fs_nodes[file_index].size;
    if (editor_length >= FS_DATA_SIZE) editor_length = FS_DATA_SIZE - 1;
    for (size_t i = 0; i < editor_length; i++) editor_buffer[i] = fs_nodes[file_index].data[i];
    editor_buffer[editor_length] = '\0';
    editor_position = editor_length;
    editor_scroll_line = 0;
    editor_dirty = 0;
    active_window = 2;
}

static void restore_terminal_screen(void) {
    duck_vga_clear(color);
    size_t rows = terminal_rows < VGA_HEIGHT ? terminal_rows : VGA_HEIGHT;
    size_t first = terminal_rows - rows;
    for (size_t row = 0; row < rows; row++) {
        const char *line = terminal_output[first + row];
        for (size_t column = 0; line[column] && column < VGA_WIDTH; column++) {
            duck_vga_write(line[column], color);
        }
        if (row + 1 < rows) duck_vga_write('\n', color);
    }
}

static void render_text_editor(const char *name, int saved) {
    duck_vga_clear(0x07);
    duck_vga_write(' ', 0x07);
    const char *prefix = "MSPC etxt 1.1 | ";
    for (size_t i = 0; prefix[i]; i++) duck_vga_cell((uint16_t)i, 0, prefix[i], 0x1f);
    size_t x = string_length(prefix);
    for (size_t i = 0; name[i] && x < VGA_WIDTH - 10; i++, x++) duck_vga_cell((uint16_t)x, 0, name[i], 0x1f);
    const char *state = saved ? " [saved]" : " [modified]";
    for (size_t i = 0; state[i] && x < VGA_WIDTH; i++, x++) duck_vga_cell((uint16_t)x, 0, state[i], 0x1f);

    size_t cursor_row = 1;
    size_t cursor_column = 0;
    for (size_t i = 0; i < editor_position; i++) {
        if (editor_buffer[i] == '\n') {
            cursor_row++;
            cursor_column = 0;
        } else if (++cursor_column == VGA_WIDTH) {
            cursor_row++;
            cursor_column = 0;
        }
    }
    size_t scroll = cursor_row > VGA_HEIGHT - 3 ? cursor_row - (VGA_HEIGHT - 3) : 1;
    size_t row = 1;
    size_t column = 0;
    for (size_t i = 0; i <= editor_length; i++) {
        if (i == editor_position && row >= scroll && row < VGA_HEIGHT - 1) {
            duck_vga_cell((uint16_t)column, (uint16_t)(row - scroll + 1), '_', 0x0f);
        }
        if (i == editor_length) break;
        char character = editor_buffer[i];
        if (character == '\n') {
            row++;
            column = 0;
        } else {
            if (row >= scroll && row < VGA_HEIGHT - 1) {
                duck_vga_cell((uint16_t)column, (uint16_t)(row - scroll + 1), character, 0x07);
            }
            if (++column == VGA_WIDTH) {
                row++;
                column = 0;
            }
        }
    }
    const char *help = "Ctrl+S save | Ctrl+Q quit | Arrows move | Ins toggle insert";
    for (size_t i = 0; help[i] && i < VGA_WIDTH; i++) duck_vga_cell((uint16_t)i, VGA_HEIGHT - 1, help[i], 0x70);
}

static void save_editor_file(int file_index) {
    for (size_t i = 0; i <= editor_length; i++) fs_nodes[file_index].data[i] = editor_buffer[i];
    fs_nodes[file_index].size = editor_length;
    editor_dirty = 0;
}

static void edit_file(const char *name) {
    int file_index = resolve_path(name);
    if (file_index < 0) file_index = create_node(name, 0);
    if (file_index < 0) {
        print("Cannot create file: invalid path or filesystem is full.\n");
        return;
    }
    if (fs_nodes[file_index].is_directory) {
        print("Cannot edit a directory.\n");
        return;
    }
    if (!has_access(file_index, ACCESS_WRITE)) {
        print("Permission denied.\n");
        return;
    }

    editor_file = file_index;
    editor_length = fs_nodes[file_index].size;
    if (editor_length >= FS_DATA_SIZE) editor_length = FS_DATA_SIZE - 1;
    for (size_t i = 0; i < editor_length; i++) editor_buffer[i] = fs_nodes[file_index].data[i];
    editor_buffer[editor_length] = '\0';
    editor_position = editor_length;
    editor_dirty = 0;
    int insert_mode = 1;
    int control_down = 0;
    render_text_editor(name, 1);

    for (;;) {
        uint8_t scan, extended, released;
        if (!duck_ps2_read_key(&scan, &extended, &released)) continue;
        if (!extended && scan == 0x1d) {
            control_down = !released;
            continue;
        }
        if (released) continue;
        if (control_down && scan == 0x1f) {
            save_editor_file(file_index);
            render_text_editor(name, 1);
            continue;
        }
        if (control_down && scan == 0x10) {
            duck_vga_clear(0x07);
            duck_vga_write('S', 0x07); duck_vga_write('a', 0x07); duck_vga_write('v', 0x07);
            duck_vga_write('e', 0x07); duck_vga_write(' ', 0x07); duck_vga_write('c', 0x07);
            duck_vga_write('h', 0x07); duck_vga_write('a', 0x07); duck_vga_write('n', 0x07);
            duck_vga_write('g', 0x07); duck_vga_write('e', 0x07); duck_vga_write('s', 0x07);
            duck_vga_write('?', 0x07); duck_vga_write(' ', 0x07);
            duck_vga_write('Y', 0x07); duck_vga_write('/', 0x07); duck_vga_write('N', 0x07);
            duck_vga_write('/', 0x07); duck_vga_write('C', 0x07); duck_vga_write(' ', 0x07);
            char answer = 0;
            while (answer != 'y' && answer != 'Y' && answer != 'n' && answer != 'N' && answer != 'c' && answer != 'C') {
                int key = duck_ps2_read_char();
                if (key >= 0) answer = (char)key;
            }
            if (answer == 'y' || answer == 'Y') save_editor_file(file_index);
            else if (answer != 'n' && answer != 'N') {
                render_text_editor(name, !editor_dirty);
                continue;
            }
            restore_terminal_screen();
            if (answer == 'n' || answer == 'N') print("Changes discarded.\n");
            else print("File saved.\n");
            return;
        }
        if (control_down) continue;

        if (extended) {
            if (scan == 0x4b && editor_position) editor_position--;
            else if (scan == 0x4d && editor_position < editor_length) editor_position++;
            else if (scan == 0x47) {
                while (editor_position && editor_buffer[editor_position - 1] != '\n') editor_position--;
            } else if (scan == 0x4f) {
                while (editor_position < editor_length && editor_buffer[editor_position] != '\n') editor_position++;
            } else if (scan == 0x48 || scan == 0x50) {
                size_t start = editor_position;
                while (start && editor_buffer[start - 1] != '\n') start--;
                size_t column = editor_position - start;
                if (scan == 0x48 && start) {
                    size_t end = start - 1;
                    size_t previous = end;
                    while (previous && editor_buffer[previous - 1] != '\n') previous--;
                    size_t width = end - previous;
                    editor_position = previous + (column < width ? column : width);
                } else if (scan == 0x50) {
                    size_t end = editor_position;
                    while (end < editor_length && editor_buffer[end] != '\n') end++;
                    if (end < editor_length) {
                        size_t next = end + 1;
                        size_t next_end = next;
                        while (next_end < editor_length && editor_buffer[next_end] != '\n') next_end++;
                        size_t width = next_end - next;
                        editor_position = next + (column < width ? column : width);
                    }
                }
            } else if (scan == 0x52) insert_mode = !insert_mode;
            else if (scan == 0x53 && editor_position < editor_length) {
                for (size_t i = editor_position; i < editor_length; i++) editor_buffer[i] = editor_buffer[i + 1];
                editor_length--;
                editor_dirty = 1;
            }
            render_text_editor(name, !editor_dirty);
            continue;
        }

        char character = duck_ps2_translate(scan);
        if (character == '\b' && editor_position) {
            for (size_t i = editor_position; i < editor_length; i++) editor_buffer[i - 1] = editor_buffer[i];
            editor_position--;
            editor_length--;
            editor_buffer[editor_length] = '\0';
            editor_dirty = 1;
        } else if (character == '\n' && editor_length + 1 < FS_DATA_SIZE) {
            for (size_t i = editor_length; i > editor_position; i--) editor_buffer[i] = editor_buffer[i - 1];
            editor_buffer[editor_position++] = '\n';
            editor_length++;
            editor_buffer[editor_length] = '\0';
            editor_dirty = 1;
        } else if (character >= ' ' && editor_length + 1 < FS_DATA_SIZE) {
            if (insert_mode) {
                for (size_t i = editor_length; i > editor_position; i--) editor_buffer[i] = editor_buffer[i - 1];
                editor_buffer[editor_position++] = character;
                editor_length++;
            } else if (editor_position < editor_length) {
                editor_buffer[editor_position++] = character;
            } else {
                editor_buffer[editor_position++] = character;
                editor_length++;
            }
            editor_buffer[editor_length] = '\0';
            editor_dirty = 1;
        }
        render_text_editor(name, !editor_dirty);
    }
}

static void run_command(char *line) {
    char *argument = line;
    while (*argument && *argument != ' ') {
        argument++;
    }
    if (*argument) {
        *argument++ = '\0';
        while (*argument == ' ') argument++;
    }

    if (!*line) return;
    if (equal(line, "help")) {
        print("MSPC command line:\n");
        for (size_t i = 0; i < sizeof(command_names) / sizeof(command_names[0]); i++) {
            print(command_names[i]);
            print(i % 6 == 5 ? "\n" : "  ");
        }
        print("\n\nFiles: ls [path], cd [path], pwd, mkdir <path>, touch <file>\n");
        print("       cat <file>, write <file> <text>, etxt <file>\n");
        print("       cp <src> <dst>, mv <path> <new-name>, rm <path>, tree [path]\n");
        print("Other: calc <a> <+|-|*|/> <b>, hex <decimal>, dec <hex>, history\n");
        print("Users: useradd <name>, users, su <name>, passwd [name]\n");
        print("       root grant|revoke <name>, chmod <octal> <path>\n");
        print("Drivers: drivers, driver load <manifest.drv>\n");
        print("Network: net, ifconfig, dns <host>, ping <IPv4-or-host>\n");
        print("Download: download http://<host>/<path> <output-file>\n");
        print("Graphics: gltest, 3dtest, gfx\n");
        print("Programs: etxt <file>, then run <file>\n");
        print("Script: print, let, show, add, sub, mul, div, input, if, goto, cat, write, append, ping, dns, end\n");
        print("RAM files and accounts are lost after reboot.\n");
    } else if (equal(line, "clear")) {
        clear_screen();
    } else if (equal(line, "echo")) {
        print(argument);
        put_char('\n');
    } else if (equal(line, "about") || equal(line, "uname")) {
        print("MSPC OS 1.1 - Copyright (C) 2026 DMYTRIY (Licensed under GNU GPLv2+)\n");
    } else if (equal(line, "ver")) {
        print("MSPC OS 1.1 - Copyright (C) 2026 DMYTRIY (Licensed under GNU GPLv2+)\n");
    } else if (equal(line, "run")) {
        char *path = next_token(&argument);
        if (!*path) print("Usage: run <program-file>\n");
        else {
            int file = resolve_or_report(path);
            if (file >= 0) execute_program(file);
        }
    } else if (equal(line, "net") || equal(line, "ifconfig")) {
        if (!duck_network_init()) print("PCnet adapter not found or failed to initialize.\n");
        else {
            uint8_t address[4], mask[4], gateway[4], dns[4];
            duck_network_get_config(address, mask, gateway, dns);
            print("PCnet link configured. IP ");
            for (size_t i = 0; i < 4; i++) { if (i) put_char('.'); print_unsigned(address[i], 10); }
            print(" / ");
            for (size_t i = 0; i < 4; i++) { if (i) put_char('.'); print_unsigned(mask[i], 10); }
            print(" gateway ");
            for (size_t i = 0; i < 4; i++) { if (i) put_char('.'); print_unsigned(gateway[i], 10); }
            print(" dns ");
            for (size_t i = 0; i < 4; i++) { if (i) put_char('.'); print_unsigned(dns[i], 10); }
            print(" mac ");
            const uint8_t *mac = duck_pcnet_mac();
            for (size_t i = 0; i < 6; i++) {
                if (i) put_char(':');
                if (mac[i] < 16) put_char('0');
                print_unsigned(mac[i], 16);
            }
            put_char('\n');
        }
    } else if (equal(line, "dns")) {
        char *hostname = next_token(&argument);
        uint8_t address[4];
        if (!*hostname) print("Usage: dns <hostname>\n");
        else if (!duck_network_resolve(hostname, address)) print("Name resolution failed.\n");
        else {
            print(hostname); print(" -> ");
            for (size_t i = 0; i < 4; i++) { if (i) put_char('.'); print_unsigned(address[i], 10); }
            put_char('\n');
        }
    } else if (equal(line, "gltest") || equal(line, "3dtest") || equal(line, "gfx")) {
        mspc_gl_run();
        mspc_gl_clear();
        print("Returned from graphics view.\n");
    } else if (equal(line, "download")) {
        char *url = next_token(&argument);
        char *output_name = next_token(&argument);
        if (!*url || !*output_name) {
            print("Usage: download http://<host>/<path> <output-file>\n");
        } else if (!(url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' &&
                     url[4] == ':' && url[5] == '/' && url[6] == '/')) {
            print("Only http:// URLs are supported; HTTPS is not implemented.\n");
        } else {
            char host[64];
            char path[128];
            char *position = url + 7;
            size_t host_length = 0;
            while (*position && *position != '/') {
                if (host_length + 1 >= sizeof(host)) break;
                host[host_length++] = *position++;
            }
            host[host_length] = '\0';
            if (!host_length) {
                print("Invalid URL host.\n");
            } else {
                size_t path_length = 0;
                if (!*position) {
                    path[path_length++] = '/';
                } else {
                    while (*position && path_length + 1 < sizeof(path)) path[path_length++] = *position++;
                }
                path[path_length] = '\0';

                size_t received = 0;
                int result = duck_network_http_get(host, path, download_buffer,
                                                   sizeof(download_buffer), &received);
                if (result < 0) {
                    print("HTTP download failed.\n");
                } else {
                    int file = resolve_path(output_name);
                    if (file < 0) file = create_node(output_name, 0);
                    if (file < 0) {
                        print("Cannot create output file.\n");
                    } else if (!has_access(file, ACCESS_WRITE) || fs_nodes[file].is_directory) {
                        print("Cannot write output file.\n");
                    } else {
                        size_t copy = received < FS_DATA_SIZE ? received : FS_DATA_SIZE - 1;
                        for (size_t i = 0; i < copy; i++) fs_nodes[file].data[i] = (char)download_buffer[i];
                        fs_nodes[file].data[copy] = '\0';
                        fs_nodes[file].size = copy;
                        print("Downloaded "); print_unsigned((uint32_t)copy, 10); print(" bytes to ");
                        print(output_name); put_char('\n');
                    }
                }
            }
        }
    } else if (equal(line, "ping")) {
        char *address = next_token(&argument);
        if (!*address) print("Usage: ping <IPv4-address>\n");
        else {
            print("PING "); print(address); print(": ");
            int result = duck_network_ping(address);
            if (result == 1) print("reply received\n");
            else if (result == 0) print("request timed out\n");
            else if (result == -1) print("invalid IPv4 address\n");
            else if (result == -2) print("PCnet unavailable\n");
            else if (result == -3) print("ARP resolution failed\n");
            else if (result == -5) print("DNS lookup failed\n");
            else print("packet transmit failed\n");
        }
    } else if (equal(line, "etxt")) {
        char *name = next_token(&argument);
        if (!*name) {
            print("Usage: etxt <filename>\n");
        } else {
            edit_file(name);
        }
    } else if (equal(line, "users")) {
        for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
            if (!accounts[i].used) continue;
            print(i == current_account ? "* " : "  ");
            print(accounts[i].name);
            print(accounts[i].is_root ? " [root]\n" : " [user]\n");
        }
    } else if (equal(line, "useradd")) {
        char *name = next_token(&argument);
        if (!current_is_root()) print("Only root can create accounts.\n");
        else if (!valid_account_name(name)) print("Usage: useradd <lowercase-name>\n");
        else if (find_account(name) >= 0) print("Account already exists.\n");
        else {
            int slot = -1;
            for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
                if (!accounts[i].used) { slot = (int)i; break; }
            }
            if (slot < 0) print("Account table is full.\n");
            else {
                char password[32];
                print("New password: ");
                read_keyboard_line(password, sizeof(password), 1);
                if (!password[0]) print("Password cannot be empty.\n");
                else {
                    accounts[slot].used = 1;
                    accounts[slot].is_root = 0;
                    accounts[slot].password_hash = hash_password(password);
                    string_copy(accounts[slot].name, name, sizeof(accounts[slot].name));
                    account_count++;
                    print("Account created.\n");
                }
            }
        }
    } else if (equal(line, "passwd")) {
        char *name = next_token(&argument);
        int target = *name ? find_account(name) : current_account;
        if (target < 0) print("Account not found.\n");
        else if (target != current_account && !current_is_root()) print("Only root can change another user's password.\n");
        else {
            char password[32];
            print("New password: ");
            read_keyboard_line(password, sizeof(password), 1);
            if (!password[0]) print("Password cannot be empty.\n");
            else {
                accounts[target].password_hash = hash_password(password);
                print("Password changed.\n");
            }
        }
    } else if (equal(line, "su")) {
        char *name = next_token(&argument);
        int target = find_account(name);
        if (target < 0) print("Account not found.\n");
        else {
            char password[32];
            print("Password: ");
            read_keyboard_line(password, sizeof(password), 1);
            if (accounts[target].password_hash != hash_password(password)) print("Authentication failed.\n");
            else {
                current_account = (uint8_t)target;
                print("Switched to "); print(accounts[target].name); put_char('\n');
            }
        }
    } else if (equal(line, "root")) {
        char *action = next_token(&argument);
        char *name = next_token(&argument);
        int target = find_account(name);
        if (!current_is_root()) print("Only root can grant or revoke root.\n");
        else if ((!equal(action, "grant") && !equal(action, "revoke")) || target < 0) {
            print("Usage: root grant|revoke <user>\n");
        } else if (equal(action, "grant")) {
            accounts[target].is_root = 1;
            print("Root access granted.\n");
        } else if (!accounts[target].is_root) {
            print("Account is not root.\n");
        } else {
            size_t root_count = 0;
            for (size_t i = 0; i < ACCOUNT_COUNT; i++) {
                if (accounts[i].used && accounts[i].is_root) root_count++;
            }
            if (root_count <= 1) print("Cannot revoke the last root account.\n");
            else {
                accounts[target].is_root = 0;
                print("Root access revoked.\n");
            }
        }
    } else if (equal(line, "chmod")) {
        char *mode_text = next_token(&argument);
        char *path = next_token(&argument);
        int valid;
        uint32_t mode = parse_number(mode_text, 8, &valid);
        int node = *path ? resolve_or_report(path) : -1;
        if (!current_is_root()) print("Only root can change permissions.\n");
        else if (!*mode_text || !*path) print("Usage: chmod <octal-mode> <path>\n");
        else if (!valid || mode > 0777) print("Mode must be between 000 and 777.\n");
        else if (node >= 0) fs_nodes[node].mode = (uint16_t)mode;
    } else if (equal(line, "drivers")) {
        print("Driver manifests in /drivers:\n");
        for (size_t i = 0; i < sizeof(driver_registry) / sizeof(driver_registry[0]); i++) {
            print(driver_registry[i].manifest_name);
            print(driver_registry[i].loaded ? " [loaded]\n" : " [not loaded]\n");
        }
    } else if (equal(line, "driver")) {
        char *action = next_token(&argument);
        char *name = next_token(&argument);
        if (!equal(action, "load") || !*name) {
            print("Usage: driver load <manifest.drv>\n");
        } else if (!current_is_root()) {
            print("Only root can load drivers.\n");
        } else {
            char path[FS_PATH_SIZE] = "/drivers/";
            string_copy(path + 9, name, sizeof(path) - 9);
            int manifest = resolve_or_report(path);
            if (manifest >= 0 && !has_access(manifest, ACCESS_READ)) print("Permission denied.\n");
            else if (manifest >= 0 && fs_nodes[manifest].is_directory) print("Not a driver manifest.\n");
            else if (manifest >= 0) {
                int result = activate_driver(name, (const uint8_t *)fs_nodes[manifest].data, fs_nodes[manifest].size);
                if (result == 1) print("Driver loaded.\n");
                else if (result == 2) print("Driver is already loaded.\n");
                else print("Unknown, disabled, or failed driver manifest.\n");
            }
        }
    } else if (equal(line, "ls")) {
        char *path = next_token(&argument);
        int directory = resolve_or_report(*path ? path : ".");
        if (directory >= 0) {
            if (!has_access(directory, ACCESS_READ)) print("Permission denied.\n");
            else if (!fs_nodes[directory].is_directory) print("Not a directory.\n");
            else list_directory(directory);
        }
    } else if (equal(line, "cat")) {
        char *path = next_token(&argument);
        int file = *path ? resolve_or_report(path) : -1;
        if (!*path) print("Usage: cat <file>\n");
        else if (file >= 0 && !has_access(file, ACCESS_READ)) print("Permission denied.\n");
        else if (file >= 0 && fs_nodes[file].is_directory) print("Cannot read a directory.\n");
        else if (file >= 0) {
            for (size_t i = 0; i < fs_nodes[file].size; i++) put_char(fs_nodes[file].data[i]);
            if (!fs_nodes[file].size || fs_nodes[file].data[fs_nodes[file].size - 1] != '\n') put_char('\n');
        }
    } else if (equal(line, "pwd")) {
        print_working_directory();
    } else if (equal(line, "cd")) {
        char *path = next_token(&argument);
        int directory = resolve_path(*path ? path : "/");
        if (directory < 0) print("Path not found.\n");
        else if (!fs_nodes[directory].is_directory) print("Not a directory.\n");
        else if (!has_access(directory, ACCESS_EXEC)) print("Permission denied.\n");
        else current_directory = directory;
    } else if (equal(line, "mkdir") || equal(line, "touch")) {
        uint8_t is_directory = equal(line, "mkdir");
        char *path = next_token(&argument);
        if (!*path) {
            print(is_directory ? "Usage: mkdir <path>\n" : "Usage: touch <path>\n");
        } else {
            int existing = resolve_path(path);
            if (existing >= 0 && !is_directory && !fs_nodes[existing].is_directory) {
                print("File already exists.\n");
            } else if (existing >= 0) {
                print("Path already exists.\n");
            } else if (create_node(path, is_directory) < 0) {
                print("Cannot create path: filesystem full or invalid parent.\n");
            }
        }
    } else if (equal(line, "write")) {
        char *path = next_token(&argument);
        while (*argument == ' ') argument++;
        if (!*path) {
            print("Usage: write <file> <text>\n");
        } else {
            int file = resolve_path(path);
            if (file < 0) file = create_node(path, 0);
            if (file < 0) print("Cannot create file.\n");
            else if (!has_access(file, ACCESS_WRITE)) print("Permission denied.\n");
            else if (fs_nodes[file].is_directory) print("Cannot write to a directory.\n");
            else if (string_length(argument) >= FS_DATA_SIZE) print("Text is too long.\n");
            else {
                string_copy(fs_nodes[file].data, argument, FS_DATA_SIZE);
                fs_nodes[file].size = string_length(argument);
            }
        }
    } else if (equal(line, "rm")) {
        char *path = next_token(&argument);
        int node = *path ? resolve_or_report(path) : -1;
        if (!*path) print("Usage: rm <path>\n");
        else if (node == 0) print("Cannot remove root.\n");
        else if (node > 0) {
            if (!has_access(fs_nodes[node].parent, ACCESS_WRITE)) {
                print("Permission denied.\n");
                return;
            }
            int has_children = 0;
            for (size_t i = 1; i < FS_NODE_COUNT; i++) {
                if (fs_nodes[i].used && fs_nodes[i].parent == node) has_children = 1;
            }
            if (has_children) print("Directory is not empty.\n");
            else {
                fs_nodes[node].used = 0;
                if (current_directory == node) current_directory = fs_nodes[node].parent;
            }
        }
    } else if (equal(line, "cp")) {
        char *source = next_token(&argument);
        char *destination = next_token(&argument);
        int source_node = *source ? resolve_or_report(source) : -1;
        if (!*source || !*destination) print("Usage: cp <source> <destination>\n");
        else if (source_node >= 0 && fs_nodes[source_node].is_directory) print("cp supports files only.\n");
        else if (source_node >= 0 && !has_access(source_node, ACCESS_READ)) print("Permission denied.\n");
        else if (source_node >= 0) {
            int destination_node = resolve_path(destination);
            if (destination_node >= 0 && fs_nodes[destination_node].is_directory) {
                print("Destination must be a new file path.\n");
            } else {
                if (destination_node < 0) destination_node = create_node(destination, 0);
                if (destination_node < 0) print("Cannot create destination.\n");
                else if (!has_access(destination_node, ACCESS_WRITE)) print("Permission denied.\n");
                else {
                    for (size_t i = 0; i <= fs_nodes[source_node].size; i++) {
                        fs_nodes[destination_node].data[i] = fs_nodes[source_node].data[i];
                    }
                    fs_nodes[destination_node].size = fs_nodes[source_node].size;
                }
            }
        }
    } else if (equal(line, "mv")) {
        char *source = next_token(&argument);
        char *destination = next_token(&argument);
        int node = *source ? resolve_or_report(source) : -1;
        if (!*source || !*destination) print("Usage: mv <path> <new-name>\n");
        else if (node == 0) print("Cannot move root.\n");
        else if (node > 0 && (!has_access(node, ACCESS_WRITE) || !has_access(fs_nodes[node].parent, ACCESS_WRITE))) {
            print("Permission denied.\n");
        }
        else if (node > 0 && (destination[0] == '/' || destination[0] == '.' ||
                              destination[0] == '\\' || find_child(fs_nodes[node].parent, destination) >= 0)) {
            print("Destination must be an unused name in the same directory.\n");
        } else if (node > 0 && string_length(destination) < FS_NAME_SIZE) {
            string_copy(fs_nodes[node].name, destination, FS_NAME_SIZE);
        } else if (node > 0) print("Name is too long.\n");
    } else if (equal(line, "tree")) {
        char *path = next_token(&argument);
        int directory = resolve_or_report(*path ? path : ".");
        if (directory >= 0 && !fs_nodes[directory].is_directory) print("Not a directory.\n");
        else if (directory >= 0) print_tree(directory, 0);
    } else if (equal(line, "df")) {
        size_t used_nodes = 0;
        size_t used_bytes = 0;
        for (size_t i = 1; i < FS_NODE_COUNT; i++) {
            if (fs_nodes[i].used) {
                used_nodes++;
                used_bytes += fs_nodes[i].size;
            }
        }
        print("RAMFS entries: "); print_unsigned((uint32_t)used_nodes, 10); print("/15\nData bytes: ");
        print_unsigned((uint32_t)used_bytes, 10); print("/15360\n");
    } else if (equal(line, "mem")) {
        print("RAM used by filesystem and editor: ");
        print_unsigned((uint32_t)(sizeof(fs_nodes) + sizeof(editor_buffer)), 10);
        print(" bytes (static allocation).\n");
    } else if (equal(line, "cpu")) {
        uint32_t eax = 0, ebx, ecx, edx;
        __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
        print("CPU vendor: ");
        for (size_t i = 0; i < 4; i++) put_char((char)(ebx >> (i * 8)));
        for (size_t i = 0; i < 4; i++) put_char((char)(edx >> (i * 8)));
        for (size_t i = 0; i < 4; i++) put_char((char)(ecx >> (i * 8)));
        put_char('\n');
    } else if (equal(line, "calc")) {
        char *left_text = next_token(&argument);
        char *operator = next_token(&argument);
        char *right_text = next_token(&argument);
        int left_valid, right_valid;
        uint32_t left = parse_number(left_text, 10, &left_valid);
        uint32_t right = parse_number(right_text, 10, &right_valid);
        if (!left_valid || !right_valid || !operator[0] || operator[1]) print("Usage: calc <number> <+|-|*|/> <number>\n");
        else if (operator[0] == '+') print_unsigned(left + right, 10);
        else if (operator[0] == '-') {
            if (left < right) { put_char('-'); print_unsigned(right - left, 10); }
            else print_unsigned(left - right, 10);
        } else if (operator[0] == '*') print_unsigned(left * right, 10);
        else if (operator[0] == '/' && right) print_unsigned(left / right, 10);
        else print("Invalid operation or division by zero.");
        put_char('\n');
    } else if (equal(line, "hex")) {
        char *value_text = next_token(&argument);
        int valid;
        uint32_t value = parse_number(value_text, 10, &valid);
        if (!valid) print("Usage: hex <decimal-number>\n");
        else { print("0x"); print_unsigned(value, 16); put_char('\n'); }
    } else if (equal(line, "dec")) {
        char *value_text = next_token(&argument);
        if (value_text[0] == '0' && (value_text[1] == 'x' || value_text[1] == 'X')) value_text += 2;
        int valid;
        uint32_t value = parse_number(value_text, 16, &valid);
        if (!valid) print("Usage: dec <hex-number>\n");
        else { print_unsigned(value, 10); put_char('\n'); }
    } else if (equal(line, "whoami")) {
        print(accounts[current_account].name);
        print(current_is_root() ? " (root)\n" : " (user)\n");
    } else if (equal(line, "env")) {
        print("USER=mspc\nSHELL=mspcsh\nPWD=");
        print_working_directory();
    } else if (equal(line, "history")) {
        size_t first = history_count == HISTORY_COUNT ? history_next : 0;
        for (size_t i = 0; i < history_count; i++) {
            size_t index = (first + i) % HISTORY_COUNT;
            print_unsigned((uint32_t)(i + 1), 10);
            print("  ");
            print(command_history[index]);
            put_char('\n');
        }
        if (!history_count) print("History is empty.\n");
    } else if (equal(line, "colors")) {
        color = color == 0x0f ? 0x0a : 0x0f;
        print("Console color changed.\n");
    } else if (equal(line, "reboot")) {
        print("Rebooting...\n");
        outb(0x64, 0xFE);
        for (;;) __asm__ volatile ("hlt");
    } else if (equal(line, "uname")) {
        print("MSPC\n");
    } else {
        print("Unknown command. Type help.\n");
    }
}

static void draw_cell(size_t x, size_t y, char character, uint8_t attribute) {
    duck_vga_cell((uint16_t)x, (uint16_t)y, character, attribute);
}

static void draw_text(size_t x, size_t y, const char *text, size_t width, uint8_t attribute) {
    for (size_t i = 0; text[i] && i < width; i++) draw_cell(x + i, y, text[i], attribute);
}

static void draw_unsigned(size_t x, size_t y, uint32_t value, size_t width, uint8_t attribute) {
    char digits[12];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits));
    for (size_t i = 0; i < width; i++) draw_cell(x + i, y, ' ', attribute);
    for (size_t i = 0; i < count && i < width; i++) {
        draw_cell(x + width - 1 - i, y, digits[i], attribute);
    }
}

static void draw_window(size_t x, size_t y, size_t width, size_t height,
                        const char *title, int focused) {
    uint8_t frame_attribute = 0x08;
    uint8_t title_attribute = focused ? (color == 0x0a ? 0x2f : 0x1f) : 0x70;
    for (size_t column = 0; column < width; column++) {
        draw_cell(x + column, y, column == 0 || column == width - 1 ? '+' : '-', frame_attribute);
        draw_cell(x + column, y + height - 1, column == 0 || column == width - 1 ? '+' : '-', frame_attribute);
    }
    for (size_t row = 1; row + 1 < height; row++) {
        draw_cell(x, y + row, '|', frame_attribute);
        draw_cell(x + width - 1, y + row, '|', frame_attribute);
        for (size_t column = 1; column + 1 < width; column++) draw_cell(x + column, y + row, ' ', 0x07);
    }
    draw_text(x + 2, y, title, width - 4, title_attribute);
}

static int explorer_child_at(size_t wanted) {
    size_t index = 0;
    for (size_t i = 1; i < FS_NODE_COUNT; i++) {
        if (!fs_nodes[i].used || fs_nodes[i].parent != explorer_directory) continue;
        if (index++ == wanted) return (int)i;
    }
    return -1;
}

static size_t explorer_child_count(void) {
    size_t count = 0;
    for (size_t i = 1; i < FS_NODE_COUNT; i++) {
        if (fs_nodes[i].used && fs_nodes[i].parent == explorer_directory) count++;
    }
    return count;
}

static void render_terminal_window(void) {
    draw_window(0, 1, 40, 16, " Terminal | mspcsh ", active_window == 0);
    size_t visible = terminal_rows < TERM_VIEW_ROWS ? terminal_rows : TERM_VIEW_ROWS;
    size_t first = terminal_rows - visible;
    for (size_t row = 0; row < visible; row++) {
        draw_text(1, 2 + row, terminal_output[first + row], TERM_VIEW_COLS, 0x07);
    }
    draw_text(1, 15, "MSPC> ", 6, 0x0b);
    size_t input_start = input_length > TERM_VIEW_COLS - 6 ? input_length - (TERM_VIEW_COLS - 6) : 0;
    draw_text(7, 15, input_line + input_start, TERM_VIEW_COLS - 6, 0x0f);
}

static void render_explorer_window(void) {
    draw_window(40, 1, 40, 16, " Files | RAMFS ", active_window == 1);
    draw_text(41, 2, "Path: /", 37, 0x0b);
    if (explorer_directory > 0) {
        draw_text(48, 2, fs_nodes[explorer_directory].name, 29, 0x0b);
    }
    size_t count = explorer_child_count();
    if (count && explorer_selection >= count) explorer_selection = count - 1;
    if (explorer_selection < explorer_scroll) explorer_scroll = explorer_selection;
    if (explorer_selection >= explorer_scroll + 12) explorer_scroll = explorer_selection - 11;
    for (size_t row = 0; row < 12; row++) {
        size_t child_index = explorer_scroll + row;
        int node = explorer_child_at(child_index);
        if (node < 0) break;
        uint8_t attribute = child_index == explorer_selection && active_window == 1 ? 0x70 : 0x07;
        draw_cell(41, 3 + row, child_index == explorer_selection ? '>' : ' ', attribute);
        draw_text(43, 3 + row, fs_nodes[node].name, 30, attribute);
        if (fs_nodes[node].is_directory) draw_cell(75, 3 + row, '/', attribute);
        else draw_unsigned(69, 3 + row, (uint32_t)fs_nodes[node].size, 6, attribute);
    }
    draw_text(41, 14, "Enter open | Backspace up", 37, 0x08);
}

static size_t editor_line_for_position(size_t position) {
    size_t line = 0;
    for (size_t i = 0; i < position && i < editor_length; i++) {
        if (editor_buffer[i] == '\n') line++;
    }
    return line;
}

static void render_editor_window(void) {
    char title[FS_NAME_SIZE + 16];
    string_copy(title, " Editor | ", sizeof(title));
    if (editor_file >= 0) {
        size_t used = string_length(title);
        string_copy(title + used, fs_nodes[editor_file].name, sizeof(title) - used);
        if (editor_dirty) {
            used = string_length(title);
            string_copy(title + used, " *", sizeof(title) - used);
        }
    } else {
        string_copy(title + string_length(title), "(no file)", sizeof(title) - string_length(title));
    }
    draw_window(0, 17, 80, 7, title, active_window == 2);

    size_t cursor_line = editor_line_for_position(editor_position);
    if (cursor_line < editor_scroll_line) editor_scroll_line = cursor_line;
    if (cursor_line >= editor_scroll_line + 5) editor_scroll_line = cursor_line - 4;
    size_t line_number = 0;
    size_t column = 0;
    size_t screen_row = 18;
    for (size_t index = 0; index <= editor_length; index++) {
        if (line_number >= editor_scroll_line && line_number < editor_scroll_line + 5 && index == editor_position) {
            draw_cell(1 + (column < 78 ? column : 77), screen_row + line_number - editor_scroll_line, '_', 0x0f);
        }
        if (index == editor_length) break;
        char character = editor_buffer[index];
        if (character == '\n') {
            line_number++;
            column = 0;
        } else {
            if (line_number >= editor_scroll_line && line_number < editor_scroll_line + 5 && column < 78) {
                draw_cell(1 + column, screen_row + line_number - editor_scroll_line, character, 0x07);
            }
            column++;
        }
    }
    draw_text(1, 23, "Ctrl+S save | F1/F2/F3 switch", 78, 0x08);
}

static void render_desktop(void) {
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) VGA_MEMORY[i] = (uint16_t)0x07 << 8 | ' ';
    draw_text(0, 0, " MSPC DESKTOP   F1 Terminal   F2 Files   F3 Editor   Tab: next ", VGA_WIDTH, 0x1f);
    render_terminal_window();
    render_explorer_window();
    render_editor_window();
    draw_text(0, 24, " RAMFS: volatile | Ctrl+S saves editor to RAM | Esc focuses terminal ", VGA_WIDTH, 0x70);
}

static void save_editor(void) {
    if (editor_file < 0) return;
    for (size_t i = 0; i <= editor_length; i++) fs_nodes[editor_file].data[i] = editor_buffer[i];
    fs_nodes[editor_file].size = editor_length;
    editor_dirty = 0;
    print("Editor saved in RAM.\n");
}

static void editor_move_vertical(int direction) {
    size_t line_start = editor_position;
    while (line_start && editor_buffer[line_start - 1] != '\n') line_start--;
    size_t column = editor_position - line_start;
    if (direction < 0 && line_start) {
        size_t previous_end = line_start - 1;
        size_t previous_start = previous_end;
        while (previous_start && editor_buffer[previous_start - 1] != '\n') previous_start--;
        size_t previous_length = previous_end - previous_start;
        editor_position = previous_start + (column < previous_length ? column : previous_length);
    } else if (direction > 0) {
        size_t line_end = editor_position;
        while (line_end < editor_length && editor_buffer[line_end] != '\n') line_end++;
        if (line_end < editor_length) {
            size_t next_start = line_end + 1;
            size_t next_end = next_start;
            while (next_end < editor_length && editor_buffer[next_end] != '\n') next_end++;
            size_t next_length = next_end - next_start;
            editor_position = next_start + (column < next_length ? column : next_length);
        }
    }
}

static void open_selected_file(void) {
    int node = explorer_child_at(explorer_selection);
    if (node < 0) return;
    if (fs_nodes[node].is_directory) {
        explorer_directory = node;
        explorer_selection = 0;
        explorer_scroll = 0;
    } else {
        open_editor_file(node);
    }
}

static void __attribute__((unused)) desktop(void) {
    print("Welcome to MSPC Desktop. Use F1/F2/F3 or Tab to switch windows.\n");
    render_desktop();
    int control_down = 0;
    int extended = 0;
    for (;;) {
        if (!(inb(0x64) & 1)) continue;
        uint8_t scan = inb(0x60);
        if (scan == 0xe0) {
            extended = 1;
            continue;
        }
        if (scan == 0x1d) {
            control_down = 1;
            continue;
        }
        if (scan == 0x9d) {
            control_down = 0;
            continue;
        }
        if (scan & 0x80) {
            extended = 0;
            continue;
        }

        if (scan == 0x3b) active_window = 0;
        else if (scan == 0x3c) active_window = 1;
        else if (scan == 0x3d) active_window = 2;
        else if (scan == 0x0f) active_window = (active_window + 1) % 3;
        else if (scan == 0x01) active_window = 0;
        else if (control_down && scan == 0x1f && active_window == 2) save_editor();
        else if (control_down && scan == 0x2d) active_window = 0;
        else if (extended) {
            if (active_window == 1) {
                size_t count = explorer_child_count();
                if (scan == 0x48 && explorer_selection) explorer_selection--;
                else if (scan == 0x50 && explorer_selection + 1 < count) explorer_selection++;
                else if (scan == 0x4b && explorer_directory > 0) {
                    explorer_directory = fs_nodes[explorer_directory].parent;
                    explorer_selection = 0;
                    explorer_scroll = 0;
                }
            } else if (active_window == 2) {
                if (scan == 0x4b && editor_position) editor_position--;
                else if (scan == 0x4d && editor_position < editor_length) editor_position++;
                else if (scan == 0x47) {
                    while (editor_position && editor_buffer[editor_position - 1] != '\n') editor_position--;
                } else if (scan == 0x4f) {
                    while (editor_position < editor_length && editor_buffer[editor_position] != '\n') editor_position++;
                } else if (scan == 0x48) editor_move_vertical(-1);
                else if (scan == 0x50) editor_move_vertical(1);
                else if (scan == 0x53 && editor_position < editor_length) {
                    for (size_t i = editor_position; i < editor_length; i++) editor_buffer[i] = editor_buffer[i + 1];
                    editor_length--;
                    editor_dirty = 1;
                }
            }
            extended = 0;
        } else if (control_down) {
            if (active_window == 1 && scan == 0x0e && explorer_directory > 0) {
                explorer_directory = fs_nodes[explorer_directory].parent;
                explorer_selection = 0;
                explorer_scroll = 0;
            }
        } else {
            char character = scancode_to_ascii(scan);
            if (active_window == 0) {
                if (character == '\n') {
                    input_line[input_length] = '\0';
                    print("MSPC> "); print(input_line); put_char('\n');
                    remember_command(input_line);
                    run_command(input_line);
                    input_length = 0;
                    input_line[0] = '\0';
                } else if (character == '\b' && input_length) {
                    input_line[--input_length] = '\0';
                } else if (character >= ' ' && input_length + 1 < MAX_LINE) {
                    input_line[input_length++] = character;
                    input_line[input_length] = '\0';
                }
            } else if (active_window == 1) {
                if (character == '\n') open_selected_file();
                else if (character == '\b' && explorer_directory > 0) {
                    explorer_directory = fs_nodes[explorer_directory].parent;
                    explorer_selection = 0;
                    explorer_scroll = 0;
                }
            } else if (active_window == 2) {
                if (character == '\b' && editor_position) {
                    for (size_t i = editor_position; i < editor_length; i++) editor_buffer[i - 1] = editor_buffer[i];
                    editor_position--;
                    editor_length--;
                    editor_buffer[editor_length] = '\0';
                    editor_dirty = 1;
                } else if (character == '\n' && editor_length + 1 < FS_DATA_SIZE) {
                    for (size_t i = editor_length; i > editor_position; i--) editor_buffer[i] = editor_buffer[i - 1];
                    editor_buffer[editor_position++] = '\n';
                    editor_length++;
                    editor_buffer[editor_length] = '\0';
                    editor_dirty = 1;
                } else if (character >= ' ' && editor_length + 1 < FS_DATA_SIZE) {
                    for (size_t i = editor_length; i > editor_position; i--) editor_buffer[i] = editor_buffer[i - 1];
                    editor_buffer[editor_position++] = character;
                    editor_length++;
                    editor_buffer[editor_length] = '\0';
                    editor_dirty = 1;
                }
            }
        }
        render_desktop();
    }
}

static void print_prompt(void) {
    print(accounts[current_account].name);
    print(current_is_root() ? "@MSPC# " : "@MSPC$ ");
}

static void command_shell(void) {
    print_prompt();
    for (;;) {
        int key = duck_ps2_read_char();
        if (key < 0) continue;
        char character = (char)key;

        if (character == '\n') {
            put_char('\n');
            input_line[input_length] = '\0';
            char command[MAX_LINE];
            string_copy(command, input_line, sizeof(command));
            remember_command(command);
            run_command(command);
            input_length = 0;
            input_line[0] = '\0';
            print_prompt();
        } else if (character == '\b') {
            if (input_length) {
                input_line[--input_length] = '\0';
                put_char('\b');
            }
        } else if (character >= ' ' && input_length + 1 < MAX_LINE) {
            input_line[input_length++] = character;
            input_line[input_length] = '\0';
            put_char(character);
        }
    }
}

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info) {
    (void)multiboot_info;
    duck_vga_text_init();
    if (multiboot_magic != 0x36d76289) {
        print("Boot error: expected Multiboot2 loader.\n");
        for (;;) __asm__ volatile ("hlt");
    }
    if (!walk_driver_pack(0)) {
        print("Boot error: invalid driver bundle.\n");
        for (;;) __asm__ volatile ("hlt");
    }
    clear_screen();
    create_first_account();
    if (!walk_driver_pack(1)) print("Warning: could not create /drivers.\n");
    print("MSPC OS 1.1 - Copyright (C) 2026 DMYTRIY (Licensed under GNU GPLv2+)\nType help for commands.\n\n");
    command_shell();
}