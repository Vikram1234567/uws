#include "WebSocket.h"
#include "Group.h"
#include "Hub.h"

namespace uWS {

template <bool isServer>
WebSocket<isServer>::WebSocket(bool perMessageDeflate, uS::Socket *socket) : uS::Socket(std::move(*socket)) {
    compressionStatus = perMessageDeflate ? CompressionStatus::ENABLED : CompressionStatus::DISABLED;

    // if we are created in a group with sliding deflate window allocate it here
    if (Group<isServer>::from(this)->extensionOptions & SLIDING_DEFLATE_WINDOW) {
        slidingDeflateWindow = Hub::allocateDefaultCompressor(new z_stream{});
    }
}

/*
 * Frames and sends a WebSocket message.
 *
 * Hints: Consider using any of the prepare function if any of their
 * use cases match what you are trying to achieve (pub/sub, broadcast)
 *
 * Thread safe
 *
 */
template <bool isServer>
void WebSocket<isServer>::send(const char *message, size_t length, OpCode opCode, void(*callback)(WebSocket<isServer> *webSocket, void *data, bool cancelled, void *reserved), void *callbackData, bool compress) {

#ifdef UWS_THREADSAFE
    std::lock_guard<std::recursive_mutex> lockGuard(*nodeData->asyncMutex);
    if (isClosed()) {
        if (callback) {
            callback(this, callbackData, true, nullptr);
        }
        return;
    }
#endif

    const int HEADER_LENGTH = 10;

    struct TransformData {
        OpCode opCode;
        bool compress;
        WebSocket<isServer> *s;
    } transformData = {opCode, compress && compressionStatus == WebSocket<isServer>::CompressionStatus::ENABLED && opCode < 3, this};

    struct WebSocketTransformer {
        static size_t estimate(const char *data, size_t length) {
            return length + HEADER_LENGTH;
        }

        static size_t transform(const char *src, char *dst, size_t length, TransformData transformData) {
            if (transformData.compress) {
                char *deflated = Group<isServer>::from(transformData.s)->hub->deflate((char *) src, length, (z_stream *) transformData.s->slidingDeflateWindow);
                return WebSocketProtocol<WebSocket<isServer>>::formatMessage(dst, deflated, length, transformData.opCode, length, true);
            }

            return WebSocketProtocol<WebSocket<isServer>>::formatMessage(dst, src, length, transformData.opCode, length, false);
        }
    };

    sendTransformed<WebSocketTransformer>((char *) message, length, (void(*)(void *, void *, bool, void *)) callback, callbackData, transformData);
}

template <bool isServer>
uS::Socket *WebSocket<isServer>::onData(uS::Socket *s, char *data, size_t length) {
    WebSocket<isServer> *webSocket = static_cast<WebSocket<isServer> *>(s);

    webSocket->hasOutstandingPong = false;
    if (!webSocket->isShuttingDown()) {
        webSocket->cork(true);
        WebSocketProtocol<WebSocket<isServer>>::consume(data, (unsigned int) length, webSocket);
        if (!webSocket->isClosed()) {
            webSocket->cork(false);
        }
    }

    return webSocket;
}

/*
 * Immediately terminates this WebSocket. Will call onDisconnection of its Group.
 *
 * Hints: Close code will be 1006 and message will be empty.
 *
 */
template <bool isServer>
void WebSocket<isServer>::terminate() {

#ifdef UWS_THREADSAFE
    std::lock_guard<std::recursive_mutex> lockGuard(*nodeData->asyncMutex);
    if (isClosed()) {
        return;
    }
#endif

    WebSocket<isServer>::onEnd(this);
}

/*
 * Transfers this WebSocket from its current Group to specified Group.
 *
 * Receiving Group has to have called listen(uWS::TRANSFERS) prior.
 *
 * Hints: Useful to implement subprotocols on the same thread and Loop
 * or to transfer WebSockets between threads at any point (dynamic load balancing).
 *
 * Warning: From the point of call to the point of onTransfer, this WebSocket
 * is invalid and cannot be used. What you put in is not guaranteed to be what you
 * get in onTransfer, the only guaranteed consistency is passed userData is the userData
 * of given WebSocket in onTransfer. Use setUserData and getUserData to identify the WebSocket.
 */
template <bool isServer>
void WebSocket<isServer>::transfer(Group<isServer> *group) {
    Group<isServer>::from(this)->removeWebSocket(this);
    if (group->loop == Group<isServer>::from(this)->loop) {
        // fast path
        this->nodeData = group;
        Group<isServer>::from(this)->addWebSocket(this);
        Group<isServer>::from(this)->transferHandler(this);
    } else {
        // slow path
        uS::Socket::transfer((uS::NodeData *) group, [](Poll *p) {
            WebSocket<isServer> *webSocket = (WebSocket<isServer> *) p;
            Group<isServer>::from(webSocket)->addWebSocket(webSocket);
            Group<isServer>::from(webSocket)->transferHandler(webSocket);
        });
    }
}

/*
 * Immediately calls onDisconnection of its Group and begins a passive
 * WebSocket closedown handshake in the background (might succeed or not,
 * we don't care).
 *
 * Hints: Close code and message will be what you pass yourself.
 *
 */
template <bool isServer>
void WebSocket<isServer>::close(int code, const char *message, size_t length) {
    static const int MAX_CLOSE_PAYLOAD = 123;
    length = std::min<size_t>(MAX_CLOSE_PAYLOAD, length);
    setShuttingDown(true);

    char closePayload[MAX_CLOSE_PAYLOAD + 2];
    int closePayloadLength = (int) WebSocketProtocol<WebSocket<isServer>>::formatClosePayload(closePayload, code, message, length);
    send(closePayload, closePayloadLength, OpCode::CLOSE, [](WebSocket<isServer> *p, void *data, bool cancelled, void *reserved) {
        if (!cancelled) {
            p->shutdown();
        }
    });

    WebSocket<isServer>::onEnd(this, code);
}

template <bool isServer>
void WebSocket<isServer>::onEnd(uS::Socket *s, int code) {
    WebSocket<isServer> *webSocket = static_cast<WebSocket<isServer> *>(s);

    Group<isServer>::from(webSocket)->removeWebSocket(webSocket);
    Group<isServer>::from(webSocket)->disconnectionHandler(webSocket, code, nullptr, 0);

    webSocket->template closeSocket<WebSocket<isServer>>();

    while (!webSocket->messageQueue.empty()) {
        Queue::Message *message = webSocket->messageQueue.front();
        if (message->callback) {
            message->callback(nullptr, message->callbackData, true, nullptr);
        }
        webSocket->messageQueue.pop();
    }

    webSocket->nodeData->clearPendingPollChanges(webSocket);

    // remove any per-websocket zlib memory
    if (webSocket->slidingDeflateWindow) {
        // this relates to Hub::allocateDefaultCompressor
        deflateEnd((z_stream *) webSocket->slidingDeflateWindow);
        delete (z_stream *) webSocket->slidingDeflateWindow;
        webSocket->slidingDeflateWindow = nullptr;
    }
}

template <bool isServer>
bool WebSocket<isServer>::handleFragment(char *data, size_t length, unsigned int remainingBytes, int opCode, bool fin, WebSocketState *webSocketState) {
    WebSocket<isServer> *webSocket = static_cast<WebSocket<isServer> *>(webSocketState);
    Group<isServer> *group = Group<isServer>::from(webSocket);

    if (opCode < 3) {
        if (!remainingBytes && fin && !webSocket->fragmentBuffer.length()) {
            if (webSocket->compressionStatus == WebSocket<isServer>::CompressionStatus::COMPRESSED_FRAME) {
                    webSocket->compressionStatus = WebSocket<isServer>::CompressionStatus::ENABLED;
                    data = group->hub->inflate(data, length, group->maxPayload);
                    if (!data) {
                        forceClose(webSocketState);
                        return true;
                    }
            }

            if (opCode == 1 && !WebSocketProtocol<WebSocket<isServer>>::isValidUtf8((unsigned char *) data, length)) {
                forceClose(webSocketState);
                return true;
            }

            group->messageHandler(webSocket, data, length, (OpCode) opCode);
            if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                return true;
            }
        } else {
            webSocket->fragmentBuffer.append(data, length);
            if (!remainingBytes && fin) {
                length = webSocket->fragmentBuffer.length();
                if (webSocket->compressionStatus == WebSocket<isServer>::CompressionStatus::COMPRESSED_FRAME) {
                        webSocket->compressionStatus = WebSocket<isServer>::CompressionStatus::ENABLED;
                        webSocket->fragmentBuffer.append("....");
                        data = group->hub->inflate((char *) webSocket->fragmentBuffer.data(), length, group->maxPayload);
                        if (!data) {
                            forceClose(webSocketState);
                            return true;
                        }
                } else {
                    data = (char *) webSocket->fragmentBuffer.data();
                }

                if (opCode == 1 && !WebSocketProtocol<WebSocket<isServer>>::isValidUtf8((unsigned char *) data, length)) {
                    forceClose(webSocketState);
                    return true;
                }

                group->messageHandler(webSocket, data, length, (OpCode) opCode);
                if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                    return true;
                }
                webSocket->fragmentBuffer.clear();
            }
        }
    } else {
        if (!remainingBytes && fin && !webSocket->controlTipLength) {
            if (opCode == CLOSE) {
                typename WebSocketProtocol<WebSocket<isServer>>::CloseFrame closeFrame = WebSocketProtocol<WebSocket<isServer>>::parseClosePayload(data, length);
                webSocket->close(closeFrame.code, closeFrame.message, closeFrame.length);
                return true;
            } else {
                if (opCode == PING) {
                    webSocket->send(data, length, (OpCode) OpCode::PONG);
                    group->pingHandler(webSocket, data, length);
                    if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                        return true;
                    }
                } else if (opCode == PONG) {
                    group->pongHandler(webSocket, data, length);
                    if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                        return true;
                    }
                }
            }
        } else {
            webSocket->fragmentBuffer.append(data, length);
            webSocket->controlTipLength += length;

            if (!remainingBytes && fin) {
                char *controlBuffer = (char *) webSocket->fragmentBuffer.data() + webSocket->fragmentBuffer.length() - webSocket->controlTipLength;
                if (opCode == CLOSE) {
                    typename WebSocketProtocol<WebSocket<isServer>>::CloseFrame closeFrame = WebSocketProtocol<WebSocket<isServer>>::parseClosePayload(controlBuffer, webSocket->controlTipLength);
                    webSocket->close(closeFrame.code, closeFrame.message, closeFrame.length);
                    return true;
                } else {
                    if (opCode == PING) {
                        webSocket->send(controlBuffer, webSocket->controlTipLength, (OpCode) OpCode::PONG);
                        group->pingHandler(webSocket, controlBuffer, webSocket->controlTipLength);
                        if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                            return true;
                        }
                    } else if (opCode == PONG) {
                        group->pongHandler(webSocket, controlBuffer, webSocket->controlTipLength);
                        if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                            return true;
                        }
                    }
                }

                webSocket->fragmentBuffer.resize(webSocket->fragmentBuffer.length() - webSocket->controlTipLength);
                webSocket->controlTipLength = 0;
            }
        }
    }

    return false;
}

static void base64(unsigned char *src, char *dst) {
    static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 18; i += 3) {
        *dst++ = b64[(src[i] >> 2) & 63];
        *dst++ = b64[((src[i] & 3) << 4) | ((src[i + 1] & 240) >> 4)];
        *dst++ = b64[((src[i + 1] & 15) << 2) | ((src[i + 2] & 192) >> 6)];
        *dst++ = b64[src[i + 2] & 63];
    }
    *dst++ = b64[(src[18] >> 2) & 63];
    *dst++ = b64[((src[18] & 3) << 4) | ((src[19] & 240) >> 4)];
    *dst++ = b64[((src[19] & 15) << 2)];
    *dst++ = '=';
}

template <bool isServer>
void WebSocket<isServer>::upgrade(const char *secKey, const std::string& extensionsResponse, const char *subprotocol, size_t subprotocolLength) {
    Queue::Message *messagePtr;

    unsigned char shaInput[] = "XXXXXXXXXXXXXXXXXXXXXXXX258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    memcpy(shaInput, secKey, 24);
    unsigned char shaDigest[SHA_DIGEST_LENGTH];
    SHA1(shaInput, sizeof(shaInput) - 1, shaDigest);

    char upgradeBuffer[1024];
    memcpy(upgradeBuffer, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ", 97);
    base64(shaDigest, upgradeBuffer + 97);
    memcpy(upgradeBuffer + 125, "\r\n", 2);
    size_t upgradeResponseLength = 127;

    if (extensionsResponse.length() && extensionsResponse.length() < 200) {
        memcpy(upgradeBuffer + upgradeResponseLength, "Sec-WebSocket-Extensions: ", 26);
        memcpy(upgradeBuffer + upgradeResponseLength + 26, extensionsResponse.data(), extensionsResponse.length());
        memcpy(upgradeBuffer + upgradeResponseLength + 26 + extensionsResponse.length(), "\r\n", 2);
        upgradeResponseLength += 26 + extensionsResponse.length() + 2;
    }
    // select first protocol
    for (unsigned int i = 0; i < subprotocolLength; i++) {
        if (subprotocol[i] == ',') {
            subprotocolLength = i;
            break;
        }
    }
    if (subprotocolLength && subprotocolLength < 200) {
        memcpy(upgradeBuffer + upgradeResponseLength, "Sec-WebSocket-Protocol: ", 24);
        memcpy(upgradeBuffer + upgradeResponseLength + 24, subprotocol, subprotocolLength);
        memcpy(upgradeBuffer + upgradeResponseLength + 24 + subprotocolLength, "\r\n", 2);
        upgradeResponseLength += 24 + subprotocolLength + 2;
    }
    static char stamp[] = "Sec-WebSocket-Version: 13\r\nWebSocket-Server: uWebSockets\r\n\r\n";
    memcpy(upgradeBuffer + upgradeResponseLength, stamp, sizeof(stamp) - 1);
    upgradeResponseLength += sizeof(stamp) - 1;

    messagePtr = allocMessage(upgradeResponseLength, upgradeBuffer);

    bool waiting;
    if (write(messagePtr, waiting)) {
        if (!waiting) {
            freeMessage(messagePtr);
        } else {
            messagePtr->callback = nullptr;
        }
    } else {
        freeMessage(messagePtr);
    }
}

template struct WebSocket<SERVER>;
template struct WebSocket<CLIENT>;

}
