#include "StartupManager.hpp"
#include "SetupManager.hpp"
#include "ConversationManager.hpp"

StartupManager::StartupManager(SetupManager* setup, ConversationManager* conversation, QObject* parent)
    : QObject(parent), setup_(setup), conversation_(conversation) {
    connect(setup_, &SetupManager::finished, this, [this](bool success) {
        if (state_ != State::Downloading) return;
        if (success) load();
        else {
            failureStage_ = State::Downloading; state_ = State::Error; error_ = setup_->status(); emit changed();
        }
    });
    connect(conversation_, &ConversationManager::modelsLoaded, this, [this](bool success) {
        if (state_ != State::Loading) return;
        if (success) { state_ = State::Ready; error_.clear(); }
        else { failureStage_ = State::Loading; state_ = State::Error; error_ = conversation_->modelLoadError(); }
        emit changed();
    });
    connect(conversation_, &ConversationManager::modelsInvalidated, this, [this] {
        if (state_ == State::Ready) load();
    });
}
void StartupManager::begin() {
    if (state_ != State::Waiting) return;
    conversation_->requireStartupModels(); prepare();
}
void StartupManager::retry() {
    if (state_ != State::Error) return;
    prepare();
}
void StartupManager::prepare() {
    error_.clear();
    if (setup_->checkModels()) { load(); return; }
    state_ = State::Downloading; emit changed(); setup_->download();
}
void StartupManager::load() {
    state_ = State::Loading; error_.clear(); emit changed(); conversation_->preloadModels();
}
