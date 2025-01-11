#pragma once

#include "windows_emulator.hpp"
#include <ctime>

struct syscall_context
{
    windows_emulator& win_emu;
    x64_emulator& emu;
    process_context& proc;
    mutable bool write_status{true};
    mutable bool retrigger_syscall{false};
};

inline uint64_t get_syscall_argument(x64_emulator& emu, const size_t index)
{
    switch (index)
    {
    case 0:
        return emu.reg(x64_register::r10);
    case 1:
        return emu.reg(x64_register::rdx);
    case 2:
        return emu.reg(x64_register::r8);
    case 3:
        return emu.reg(x64_register::r9);
    default:
        return emu.read_stack(index + 1);
    }
}

inline bool is_uppercase(const char character)
{
    return toupper(character) == character;
}

inline bool is_syscall(const std::string_view name)
{
    return name.starts_with("Nt") && name.size() > 3 && is_uppercase(name[2]);
}

inline std::optional<uint32_t> extract_syscall_id(const exported_symbol& symbol, std::span<const std::byte> data)
{
    if (!is_syscall(symbol.name))
    {
        return std::nullopt;
    }

    constexpr auto instruction_size = 5;
    constexpr auto instruction_offset = 3;
    constexpr auto instruction_operand_offset = 1;
    constexpr auto instruction_opcode = static_cast<std::byte>(0xB8);

    const auto instruction_rva = symbol.rva + instruction_offset;

    if (data.size() < (instruction_rva + instruction_size) || data[instruction_rva] != instruction_opcode)
    {
        return std::nullopt;
    }

    uint32_t syscall_id{0};
    static_assert(sizeof(syscall_id) <= (instruction_size - instruction_operand_offset));
    memcpy(&syscall_id, data.data() + instruction_rva + instruction_operand_offset, sizeof(syscall_id));

    return syscall_id;
}

inline std::map<uint64_t, std::string> find_syscalls(const exported_symbols& exports, std::span<const std::byte> data)
{
    std::map<uint64_t, std::string> syscalls{};

    for (const auto& symbol : exports)
    {
        const auto id = extract_syscall_id(symbol, data);
        if (id)
        {
            auto& entry = syscalls[*id];

            if (!entry.empty())
            {
                throw std::runtime_error("Syscall with id " + std::to_string(*id) + ", which is mapping to " +
                                         symbol.name + ", was already mapped to " + entry);
            }

            entry = symbol.name;
        }
    }

    return syscalls;
}

inline void map_syscalls(std::map<uint64_t, syscall_handler_entry>& handlers, std::map<uint64_t, std::string> syscalls)
{
    for (auto& [id, name] : syscalls)
    {
        auto& entry = handlers[id];

        if (!entry.name.empty())
        {
            throw std::runtime_error("Syscall with id " + std::to_string(id) + ", which is mapping to " + name +
                                     ", was previously mapped to " + entry.name);
        }

        entry.name = std::move(name);
        entry.handler = nullptr;
    }
}

template <typename T>
    requires(std::is_integral_v<T> || std::is_enum_v<T>)
T resolve_argument(x64_emulator& emu, const size_t index)
{
    const auto arg = get_syscall_argument(emu, index);
    return static_cast<T>(arg);
}

template <typename T>
    requires(std::is_same_v<std::remove_cvref_t<T>, handle>)
handle resolve_argument(x64_emulator& emu, const size_t index)
{
    handle h{};
    h.bits = resolve_argument<uint64_t>(emu, index);
    return h;
}

template <typename T>
    requires(std::is_same_v<T, emulator_object<typename T::value_type>>)
T resolve_argument(x64_emulator& emu, const size_t index)
{
    const auto arg = get_syscall_argument(emu, index);
    return T(emu, arg);
}

template <typename T>
T resolve_indexed_argument(x64_emulator& emu, size_t& index)
{
    return resolve_argument<T>(emu, index++);
}

inline void write_syscall_status(const syscall_context& c, const NTSTATUS status, const uint64_t initial_ip)
{
    if (c.write_status && !c.retrigger_syscall)
    {
        c.emu.reg<uint64_t>(x64_register::rax, static_cast<uint64_t>(status));
    }

    const auto new_ip = c.emu.read_instruction_pointer();
    if (initial_ip != new_ip || c.retrigger_syscall)
    {
        c.emu.reg(x64_register::rip, new_ip - 2);
    }
}

inline void forward_syscall(const syscall_context& c, NTSTATUS (*handler)())
{
    const auto ip = c.emu.read_instruction_pointer();

    const auto ret = handler();
    write_syscall_status(c, ret, ip);
}

template <typename... Args>
void forward_syscall(const syscall_context& c, NTSTATUS (*handler)(const syscall_context&, Args...))
{
    const auto ip = c.emu.read_instruction_pointer();

    size_t index = 0;
    std::tuple<const syscall_context&, Args...> func_args{
        c, resolve_indexed_argument<std::remove_cv_t<std::remove_reference_t<Args>>>(c.emu, index)...};

    (void)index;

    const auto ret = std::apply(handler, std::move(func_args));
    write_syscall_status(c, ret, ip);
}

template <auto Handler>
syscall_handler make_syscall_handler()
{
    return +[](const syscall_context& c) { forward_syscall(c, Handler); };
}

template <typename T, typename Traits>
void write_attribute(emulator& emu, const PS_ATTRIBUTE<Traits>& attribute, const T& value)
{
    if (attribute.ReturnLength)
    {
        emulator_object<typename Traits::SIZE_T>{emu, attribute.ReturnLength}.write(sizeof(T));
    }

    if (attribute.Size >= sizeof(T))
    {
        emulator_object<T>{emu, attribute.Value}.write(value);
    }
}

constexpr auto HUNDRED_NANOSECONDS_IN_ONE_SECOND = 10000000LL;
constexpr auto EPOCH_DIFFERENCE_1601_TO_1970_SECONDS = 11644473600LL;
constexpr auto WINDOWS_EPOCH_DIFFERENCE = EPOCH_DIFFERENCE_1601_TO_1970_SECONDS * HUNDRED_NANOSECONDS_IN_ONE_SECOND;

inline std::chrono::steady_clock::time_point convert_delay_interval_to_time_point(const LARGE_INTEGER delay_interval)
{
    if (delay_interval.QuadPart <= 0)
    {
        const auto relative_time = -delay_interval.QuadPart;
        const auto relative_ticks_in_ms = relative_time / 10;
        const auto relative_fraction_ns = (relative_time % 10) * 100;
        const auto relative_duration =
            std::chrono::microseconds(relative_ticks_in_ms) + std::chrono::nanoseconds(relative_fraction_ns);

        return std::chrono::steady_clock::now() + relative_duration;
    }

    const auto delay_seconds_since_1601 = delay_interval.QuadPart / HUNDRED_NANOSECONDS_IN_ONE_SECOND;
    const auto delay_fraction_ns = (delay_interval.QuadPart % HUNDRED_NANOSECONDS_IN_ONE_SECOND) * 100;

    const auto delay_seconds_since_1970 = delay_seconds_since_1601 - EPOCH_DIFFERENCE_1601_TO_1970_SECONDS;

    const auto target_time =
        std::chrono::system_clock::from_time_t(delay_seconds_since_1970) + std::chrono::nanoseconds(delay_fraction_ns);

    const auto now_system = std::chrono::system_clock::now();

    const auto duration_until_target = std::chrono::duration_cast<std::chrono::microseconds>(target_time - now_system);

    return std::chrono::steady_clock::now() + duration_until_target;
}

inline KSYSTEM_TIME convert_to_ksystem_time(const std::chrono::system_clock::time_point& tp)
{
    const auto duration = tp.time_since_epoch();
    const auto ns_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);

    const auto total_ticks = ns_duration.count() / 100 + WINDOWS_EPOCH_DIFFERENCE;

    KSYSTEM_TIME time{};
    time.LowPart = static_cast<uint32_t>(total_ticks);
    time.High1Time = static_cast<int32_t>(total_ticks >> 32);
    time.High2Time = time.High1Time;

    return time;
}

inline void convert_to_ksystem_time(volatile KSYSTEM_TIME* dest, const std::chrono::system_clock::time_point& tp)
{
    const auto time = convert_to_ksystem_time(tp);
    memcpy(const_cast<KSYSTEM_TIME*>(dest), &time, sizeof(*dest));
}

inline std::chrono::system_clock::time_point convert_from_ksystem_time(const KSYSTEM_TIME& time)
{
    auto totalTicks = (static_cast<int64_t>(time.High1Time) << 32) | time.LowPart;
    totalTicks -= WINDOWS_EPOCH_DIFFERENCE;

    const auto duration = std::chrono::system_clock::duration(totalTicks * 100);
    return std::chrono::system_clock::time_point(duration);
}

inline std::chrono::system_clock::time_point convert_from_ksystem_time(const volatile KSYSTEM_TIME& time)
{
    return convert_from_ksystem_time(*const_cast<const KSYSTEM_TIME*>(&time));
}

#ifndef OS_WINDOWS
using __time64_t = int64_t;
#endif

inline LARGE_INTEGER convert_unix_to_windows_time(const __time64_t unix_time)
{
    LARGE_INTEGER windows_time{};
    windows_time.QuadPart = (unix_time + EPOCH_DIFFERENCE_1601_TO_1970_SECONDS) * HUNDRED_NANOSECONDS_IN_ONE_SECOND;
    return windows_time;
}
