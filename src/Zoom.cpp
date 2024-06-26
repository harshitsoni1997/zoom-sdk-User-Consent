#include "Zoom.h"
#include <future> 
#include <thread> 
#include <json/json.h>

SDKError Zoom::config(int ac, char** av) {
    auto status = m_config.read(ac, av);
    if (status) {
        Log::error("failed to read configuration");
        return SDKERR_INTERNAL_ERROR;
    }

    return SDKERR_SUCCESS;
}

SDKError Zoom::init() {
    InitParam initParam;

    auto host = m_config.zoomHost().c_str();

    initParam.strWebDomain = host;
    initParam.strSupportUrl = host;

    initParam.emLanguageID = LANGUAGE_English;

    initParam.enableLogByDefault = true;
    initParam.enableGenerateDump = true;

    auto err = InitSDK(initParam);
    if (hasError(err)) {
        Log::error("InitSDK failed");
        return err;
    }

    return createServices();
}

SDKError Zoom::createServices() {
    auto err = CreateMeetingService(&m_meetingService);
    if (hasError(err)) return err;

    err = CreateSettingService(&m_settingService);
    if (hasError(err)) return err;

    auto meetingServiceEvent = new MeetingServiceEvent();
    meetingServiceEvent->setOnMeetingJoin(onJoin);

    err = m_meetingService->SetEvent(meetingServiceEvent);
    if (hasError(err)) return err;

    return CreateAuthService(&m_authService);
}

SDKError Zoom::auth() {
    SDKError err{SDKERR_UNINITIALIZE};

    auto id = m_config.clientId();
    auto secret = m_config.clientSecret();

    if (id.empty()) {
        Log::error("Client ID cannot be blank");
        return err;
    }

    if (secret.empty()) {
        Log::error("Client Secret cannot be blank");
        return err;
    }

    err = m_authService->SetEvent(new AuthServiceEvent(onAuth));
    if (hasError(err)) return err;

    generateJWT(m_config.clientId(), m_config.clientSecret());

    AuthContext ctx;
    ctx.jwt_token =  m_jwt.c_str();

    return m_authService->SDKAuth(ctx);
}

void Zoom::generateJWT(const string& key, const string& secret) {
    m_iat = std::chrono::system_clock::now();
    m_exp = m_iat + std::chrono::hours{24};

    m_jwt = jwt::create()
            .set_type("JWT")
            .set_issued_at(m_iat)
            .set_expires_at(m_exp)
            .set_payload_claim("appKey", claim(key))
            .set_payload_claim("tokenExp", claim(m_exp))
            .sign(algorithm::hs256{secret});
}

SDKError Zoom::join() {
    SDKError err{SDKERR_UNINITIALIZE};

    auto mid = m_config.meetingId();
    auto password = m_config.password();
    auto displayName = m_config.displayName();

    if (mid.empty()) {
        Log::error("Meeting ID cannot be blank");
        return err;
    }

    if (password.empty()) {
        Log::error("Meeting Password cannot be blank");
        return err;
    }

    if (displayName.empty()) {
        Log::error("Display Name cannot be blank");
        return err;
    }

    auto meetingNumber = stoull(mid);
    auto userName = displayName.c_str();
    auto psw = password.c_str();

    JoinParam joinParam;
    joinParam.userType = ZOOM_SDK_NAMESPACE::SDK_UT_WITHOUT_LOGIN;

    JoinParam4WithoutLogin& param = joinParam.param.withoutloginuserJoin;

    param.meetingNumber = meetingNumber;
    param.userName = userName;
    param.psw = psw;
    param.vanityID = nullptr;
    param.customer_key = nullptr;
    param.webinarToken = nullptr;
    param.isVideoOff = false;
    param.isAudioOff = false;

    if (!m_config.zak().empty()) {
        Log::success("used ZAK token");
        param.userZAK = m_config.zak().c_str();
    }

    if (!m_config.joinToken().empty()) {
        Log::success("used App Privilege token");
        param.app_privilege_token = m_config.joinToken().c_str();
    }

    if (m_config.useRawAudio()) {
        auto* audioSettings = m_settingService->GetAudioSettings();
        if (!audioSettings) return SDKERR_INTERNAL_ERROR;

        audioSettings->EnableAutoJoinAudio(true);
    }

    return m_meetingService->Join(joinParam);
}

SDKError Zoom::start() {
    SDKError err;

    StartParam startParam;
    startParam.userType = SDK_UT_NORMALUSER;

    StartParam4NormalUser  normalUser;
    normalUser.vanityID = nullptr;
    normalUser.customer_key = nullptr;
    normalUser.isAudioOff = true;
    normalUser.isVideoOff = true;

    err = m_meetingService->Start(startParam);
    hasError(err, "start meeting");

    return err;
}

SDKError Zoom::leave() {
    if (!m_meetingService)
        return SDKERR_UNINITIALIZE;

    auto status = m_meetingService->GetMeetingStatus();
    if (status == MEETING_STATUS_IDLE)
        return SDKERR_WRONG_USAGE;

    return  m_meetingService->Leave(LEAVE_MEETING);
}

SDKError Zoom::clean() {
    if (m_meetingService)
        DestroyMeetingService(m_meetingService);

    if (m_settingService)
        DestroySettingService(m_settingService);

    if (m_authService)
        DestroyAuthService(m_authService);

    if (m_audioHelper)
        m_audioHelper->unSubscribe();

    if (m_videoHelper)
        m_videoHelper->unSubscribe();

    delete m_videoSource;

    return CleanUPSDK();
}

SDKError Zoom::sendConsentRequest(IMeetingChatController* chatCtrl) {
    if (!chatCtrl) {
        return SDKERR_UNINITIALIZE;
    }

    // Build the chat message
    IChatMsgInfoBuilder* msgBuilder = chatCtrl->GetChatMessageBuilder();
    if (!msgBuilder) {
        return SDKERR_UNINITIALIZE;
    }

    const char* consentMessage = "We would like to record this meeting. Please provide your consent by visiting the following link: https://testui.identifai.info/consent-form?bot_id=25aad7b0-a6a5-4c1e-b379-e44eb85e1bc7";
    msgBuilder->SetContent(consentMessage)->SetReceiver(0)->SetMessageType(SDKChatMessageType_To_All);
    IChatMsgInfo* chatMsg = msgBuilder->Build();
    if (!chatMsg) {
        return SDKERR_UNINITIALIZE;
    }

    // Send the chat message
    SDKError err = chatCtrl->SendChatMsgTo(chatMsg);
    if (err != SDKERR_SUCCESS) {
        return err;
    }

    Log::info("Consent request sent successfully");
    return SDKERR_SUCCESS;
}


SDKError Zoom::startRawRecording() {

    auto recCtrl = m_meetingService->GetMeetingRecordingController();

    SDKError err = recCtrl->CanStartRawRecording();
    if (hasError(err)) {
        Log::info("requesting local recording privilege");
        auto chatCtrl = m_meetingService->GetMeetingChatController();
        err = sendConsentRequest(chatCtrl);
        if (hasError(err, "send consent request")) {
            return err;
        }
        return recCtrl->RequestLocalRecordingPrivilege();
    }

    err = recCtrl->StartRawRecording();
    if (hasError(err, "start raw recording")) {
        return err;
    }

    if (m_config.useRawVideo()) {
        if (!m_videoSource)
            m_videoSource = new ZoomSDKRendererDelegate();

        err = createRenderer(&m_videoHelper, m_videoSource);
        if (hasError(err, "create raw video renderer")) {
            return err;
        }

        m_videoSource->setDir(m_config.videoDir());
        m_videoSource->setFilename(m_config.videoFile());

        auto participantCtl = m_meetingService->GetMeetingParticipantsController();
        auto uid = participantCtl->GetParticipantsList()->GetItem(0);

        m_videoHelper->setRawDataResolution(ZoomSDKResolution_720P);
        err = m_videoHelper->subscribe(uid, RAW_DATA_TYPE_VIDEO);
        if (hasError(err, "subscribe to raw video")) {
            return err;
        }
    }

    if (m_config.useRawAudio()) {
        m_audioHelper = GetAudioRawdataHelper();
        if (!m_audioHelper) {
            return SDKERR_UNINITIALIZE;
        }

        if (!m_audioSource) {
            m_audioSource = new ZoomSDKAudioRawDataDelegate(!m_config.separateParticipantAudio());
            m_audioSource->setDir(m_config.audioDir());
            m_audioSource->setFilename(m_config.audioFile());
        }

        err = m_audioHelper->subscribe(m_audioSource);
        if (hasError(err, "subscribe to raw audio")) {
            return err;
        }
    }

    return SDKERR_SUCCESS;
}

SDKError Zoom::stopRawRecording() {
    auto recCtrl = m_meetingService->GetMeetingRecordingController();
    auto err = recCtrl->StopRawRecording();
    hasError(err, "stop raw recording");

    return err;
}

bool Zoom::isMeetingStart() {
    return m_config.isMeetingStart();
}

bool Zoom::hasError(const SDKError e, const string& action) {
    auto isError = e != SDKERR_SUCCESS;

    if(!action.empty()) {
        if (isError) {
            stringstream ss;
            ss << "failed to " << action << " with status " << e;
            Log::error(ss.str());
        } else {
            Log::success(action);
        }
    }
    return isError;
}

// Method to fetch participants
void Zoom::fetchParticipants() {
    auto* participantsController = m_meetingService->GetMeetingParticipantsController();
    if (!participantsController) return;

    auto participantsList = participantsController->GetParticipantsList();
    if (!participantsList) return;

    participants.clear();
    for (int i = 0; i < participantsList->GetCount(); ++i) {
        unsigned int userId = participantsList->GetItem(i);
        if (userId) {
            IUserInfo* userInfo = participantsController->GetUserByUserID(userId);
            if (userInfo) {
                std::string userName = userInfo->GetUserName();
                participants.insert(userName);
            }
        }
    }
}

// Method to send a message in the chat
void Zoom::sendMessage(const std::string& message) {
    auto* chatController = m_meetingService->GetMeetingChatController();
    if (!chatController) return;

    IChatMsgInfoBuilder* msgBuilder = chatController->GetChatMessageBuilder();
    if (!msgBuilder) return;

    msgBuilder->SetContent(message.c_str())->SetReceiver(0)->SetMessageType(SDKChatMessageType_To_All);
    IChatMsgInfo* chatMsg = msgBuilder->Build();
    if (chatMsg) {
        chatController->SendChatMsgTo(chatMsg);
    }
}

void Zoom::checkConsentStatus() {
    while (!recordingStarted) {
        // Simulate API call
        std::this_thread::sleep_for(std::chrono::seconds(2)); // Adjust interval as needed
        fetchParticipants();

        // API call to fetch consent status
        std::string apiUrl = "http://localhost:5000/consent"; // Replace with actual API URL
        std::future<std::string> response = std::async(std::launch::async, [&apiUrl] {
            // Your HTTP GET request to fetch consent status
            // Perform the HTTP request here and return the response as a string
            // For demonstration purposes, a mock implementation is provided below
            // Replace this with your actual HTTP request code
            std::string dummyApiResponse = "{\"consenting_users\": [\"IdentifAI KYE\", \"Harshit Soni\"]}";
            return dummyApiResponse;
        });

        auto result = response.get();
        Json::Value jsonData;
        Json::Reader reader;
        if (reader.parse(result, jsonData)) {
            auto consentingUsers = jsonData["consenting_users"];
            std::vector<std::string> users;
            for (const auto& user : consentingUsers) {
                users.push_back(user.asString());
            }
            onConsentUpdate(users);
        }
    }
}


// Callback when consent API is called
void Zoom::onConsentUpdate(const std::vector<std::string>& consentingUsers) {
    for (const auto& user : participants) {
        consentStatus[user] = std::find(consentingUsers.begin(), consentingUsers.end(), user) != consentingUsers.end();
    }

    bool allConsented = std::all_of(participants.begin(), participants.end(), [this](const std::string& user) {
        return consentStatus[user];
    });

    if (allConsented) {
        recordingStarted = true;
        startRawRecording();
    } else {
        sendConsentReminder();
    }
}

// Send consent reminder message
void Zoom::sendConsentReminder() {
    std::string reminder = "Please provide your consent for recording.";
    for (const auto& user : participants) {
        if (!consentStatus[user]) {
            reminder += "\n- " + user;
        }
    }
    sendMessage(reminder);
}

// New function implementation
void Zoom::startRecordingIfAllConsented() {
    bool allConsented = std::all_of(consentStatus.begin(), consentStatus.end(),
                                     [](const auto& entry) { return entry.second; });

    if (allConsented) {
        Log::info("All participants have consented. Starting recording...");
        startRawRecording();
    } else {
        Log::info("Not all participants have consented yet. Waiting...");
    }
}


// Start checking for consent
void Zoom::startConsentCheck() {
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!recordingStarted) {
            sendConsentReminder();
        }
    }).detach();

    std::thread(&Zoom::checkConsentStatus, this).detach();
}
