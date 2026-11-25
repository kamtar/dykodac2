#include "app/config_service.hpp"

#include <cstring>

namespace app {
namespace {
using usb::config::Opcode;
using usb::config::Status;

bool same(const settings::Settings& a, const settings::Settings& b) noexcept {
    return std::memcmp(&a, &b, sizeof(settings::Settings)) == 0;
}

Status flash_status(FlashResult result) noexcept {
    switch (result) {
    case FlashResult::Ok: return Status::Ok;
    case FlashResult::NoRecord: return Status::NoFlashRecord;
    case FlashResult::Unavailable: return Status::FlashUnavailable;
    default: return Status::FlashError;
    }
}
} // namespace

void ConfigService::make_active(const settings::Settings& s, ConfigBackend& backend) noexcept {
    active_ = s;
    staging_ = s;
    backend.apply(active_);
}

void ConfigService::boot(ConfigBackend& backend) noexcept {
    settings::Settings loaded{};
    std::uint32_t sequence = 0U;
    last_flash_ = backend.flash_load(loaded, sequence);
    if (last_flash_ == FlashResult::Ok && settings::validate(loaded) == settings::Invalid::None) {
        flash_valid_ = true;
        flash_sequence_ = sequence;
        flash_copy_ = loaded;
        make_active(loaded, backend);
        return;
    }
    if (last_flash_ == FlashResult::Ok) last_flash_ = FlashResult::NoRecord; // CRC-good but stale layout
    make_active(settings::defaults(), backend);
}

usb::config::Response ConfigService::handle(const std::uint8_t* packet, std::size_t size,
                                            ConfigBackend& backend) noexcept {
    usb::config::Response response{};
    if (!usb::config::is_request(packet, size)) {
        response.status = static_cast<std::uint8_t>(Status::BadRequest);
        return response;
    }
    usb::config::Request request{};
    std::memcpy(&request, packet, sizeof(request));
    response.opcode = request.opcode;
    response.tag = request.tag;
    response.offset = request.offset;
    Status status = Status::Ok;

    const auto in_range = [&request]() {
        return request.length <= usb::config::chunk_bytes &&
               static_cast<std::size_t>(request.offset) + request.length <= sizeof(settings::Settings);
    };

    switch (static_cast<Opcode>(request.opcode)) {
    case Opcode::Info: {
        usb::config::Info info{};
        info.settings_size = static_cast<std::uint16_t>(sizeof(settings::Settings));
        info.settings_version = settings::version;
        info.eq_bands = static_cast<std::uint8_t>(settings::eq_band_count);
        info.flash_sequence = flash_sequence_;
        info.headroom_db_8_8 = active_.headroom_db_8_8;
        info.last_flash_status = static_cast<std::uint8_t>(last_flash_);
        if (backend.flash_available()) info.flags |= usb::config::flag_flash_available;
        if (flash_valid_) info.flags |= usb::config::flag_flash_record;
        if (!flash_valid_ || !same(active_, flash_copy_)) info.flags |= usb::config::flag_unsaved;
        if (!same(active_, staging_)) info.flags |= usb::config::flag_staging_dirty;
        backend.fill_runtime_info(info);
        std::memcpy(response.data.data(), &info, sizeof(info));
        response.length = static_cast<std::uint8_t>(sizeof(info));
        break;
    }
    case Opcode::Read:
        if (!in_range()) { status = Status::BadRange; break; }
        std::memcpy(response.data.data(), reinterpret_cast<const std::uint8_t*>(&active_) + request.offset,
                    request.length);
        response.length = request.length;
        break;
    case Opcode::Write:
        if (!in_range()) { status = Status::BadRange; break; }
        std::memcpy(reinterpret_cast<std::uint8_t*>(&staging_) + request.offset, request.data.data(),
                    request.length);
        response.length = request.length;
        break;
    case Opcode::Apply: {
        const auto invalid = settings::validate(staging_);
        response.aux = static_cast<std::uint32_t>(invalid);
        if (invalid != settings::Invalid::None) {
            staging_ = active_; // a rejected upload never lingers half-written
            status = Status::InvalidSettings;
            break;
        }
        make_active(staging_, backend);
        break;
    }
    case Opcode::Save: {
        if (!backend.flash_available()) { status = Status::FlashUnavailable; break; }
        std::uint32_t sequence = flash_sequence_;
        last_flash_ = backend.flash_save(active_, sequence);
        response.aux = sequence;
        if (last_flash_ == FlashResult::Ok) {
            flash_valid_ = true;
            flash_sequence_ = sequence;
            flash_copy_ = active_;
        }
        status = flash_status(last_flash_);
        break;
    }
    case Opcode::Load: {
        settings::Settings loaded{};
        std::uint32_t sequence = 0U;
        last_flash_ = backend.flash_load(loaded, sequence);
        if (last_flash_ == FlashResult::Ok && settings::validate(loaded) != settings::Invalid::None)
            last_flash_ = FlashResult::NoRecord;
        status = flash_status(last_flash_);
        if (status != Status::Ok) break;
        flash_valid_ = true;
        flash_sequence_ = sequence;
        flash_copy_ = loaded;
        response.aux = sequence;
        make_active(loaded, backend);
        break;
    }
    case Opcode::PeekRom: {
        std::uint32_t address = 0U;
        std::memcpy(&address, request.data.data(), sizeof(address));
        response.aux = address;
        if (request.length == 0U || request.length > usb::config::chunk_bytes ||
            !backend.peek_rom(address, response.data.data(), request.length)) { status = Status::BadRange; break; }
        response.length = request.length;
        break;
    }
    case Opcode::Defaults:
        make_active(settings::defaults(), backend);
        break;
    default:
        status = Status::BadRequest;
        break;
    }
    response.status = static_cast<std::uint8_t>(status);
    return response;
}

} // namespace app
