#pragma once

#include <vector>
#include <cstdint>

/**
 * @file IFrameClassifier.h
 * @brief Інтерфейс класифікатора вхідного кадру (§4.3 дизайну).
 *
 * Classify визначає, до якого pending-запиту (primary/service) належить
 * вхідний кадр, або що він Unsolicited/Reject. Конкретна Privat-логіка —
 * драйверна фаза (§15 дизайну), тут лише інтерфейс + класифікатор-подвійники
 * у тестах.
 */

/// Клас вхідного кадру щодо активних pending-запитів.
enum class FrameClass {
    PrimaryResponse,
    ServiceResponse,
    RejectPrimary,
    RejectService,
    RejectBoth,     ///< неоднозначний reject при обох pending → сесія в desync
    Unsolicited
};

/// Причина відхилення (classifier-local, не RequestStatus).
enum class RejectReason { Busy, Unsupported };

/// Знімок активних pending-запитів на момент класифікації.
struct PendingView {
    const std::vector<uint8_t>* primary;
    const std::vector<uint8_t>* service;
};

/// Результат класифікації кадру.
struct Classification {
    FrameClass cls;
    RejectReason reason;
};

class IFrameClassifier {
public:
    virtual ~IFrameClassifier() = default;

    /// Класифікувати вхідний кадр відносно активних pending-запитів.
    virtual Classification Classify(const PendingView& pending,
                                     const std::vector<uint8_t>& frame) = 0;
};
