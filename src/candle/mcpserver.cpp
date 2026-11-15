// This file is a part of "Candle" application.
// Copyright 2015-2025 Hayrullin Denis Ravilevich

#include "mcpserver.h"
#include "script/coordinates.h"
#include <QRegularExpression>
#include <QCoreApplication>
#include <QMetaMethod>
#include <QMetaObject>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraImageCapture>
#include <QCameraViewfinderSettings>
#include <QEventLoop>
#include <QTimer>
#include <QImage>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

static const int DEFAULT_MCP_PORT = 8765;
static const int CAMERA_START_TIMEOUT = 4000;
static const int CAMERA_CAPTURE_TIMEOUT = 5000;

McpServer::McpServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_device(nullptr)
    , m_deviceState(-1)
    , m_senderState(-1)
    , m_port(DEFAULT_MCP_PORT)
    , m_feedOverride(100)
    , m_rapidOverride(100)
    , m_spindleRpm(0)
    , m_spindleCw(true)
    , m_spindleEnabled(false)
    , m_camera(nullptr)
    , m_capture(nullptr)
{
    connect(m_server, &QTcpServer::newConnection, this, &McpServer::onNewConnection);
}

McpServer::~McpServer()
{
    releaseCamera();
    stop();
}

void McpServer::setPort(int port)
{
    if (m_port != port) {
        m_port = port;
        emit portChanged(m_port);
    }
}

void McpServer::setDevice(QObject *device)
{
    m_device = device;
}

void McpServer::setState(int deviceState, int senderState)
{
    m_deviceState = deviceState;
    m_senderState = senderState;
}

void McpServer::setFeedOverride(int feedPercent, int rapidPercent)
{
    m_feedOverride = feedPercent;
    m_rapidOverride = rapidPercent;
}

void McpServer::setSpindleState(int rpm, bool cw, bool enabled)
{
    m_spindleRpm = rpm;
    m_spindleCw = cw;
    m_spindleEnabled = enabled;
}

bool McpServer::start()
{
    if (m_server->isListening()) {
        return true;
    }

    if (!m_server->listen(QHostAddress::Any, m_port)) {
        emit message("error", QString("Failed to listen on port %1: %2").arg(m_port).arg(m_server->errorString()));
        return false;
    }

    emit message("info", QString("MCP server started on port %1").arg(m_port));
    emit runningChanged(true);
    return true;
}

void McpServer::stop()
{
    m_server->close();
    for (auto socket : m_buffers.keys()) {
        socket->disconnectFromHost();
    }
    m_buffers.clear();
    emit runningChanged(false);
}

void McpServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        m_buffers[socket] = QByteArray();

        connect(socket, &QTcpSocket::readyRead, this, &McpServer::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &McpServer::onSocketDisconnected);
    }
}

void McpServer::onSocketReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    m_buffers[socket] += socket->readAll();

    QString method, path, body;

    if (!parseHttpRequest(socket, m_buffers[socket], method, path, body)) {
        return;
    }

    Q_UNUSED(path)

    m_buffers[socket].clear();

    if (method == "POST" && !body.isEmpty()) {
        handleMcpRequest(socket, body);
    } else if (method == "GET") {
        handleHealth(socket);
    } else if (method == "OPTIONS") {
        sendJsonResponse(socket, 200, QJsonObject());
    } else {
        sendJsonResponse(socket, 404, QJsonObject{{"error", "Not found"}});
    }
}

void McpServer::onSocketDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        m_buffers.remove(socket);
        socket->deleteLater();
    }
}

bool McpServer::parseHttpRequest(QTcpSocket *socket, const QByteArray &data,
                                  QString &method, QString &path, QString &body)
{
    int sep = data.indexOf("\r\n\r\n");
    if (sep < 0) return false;

    QString header(data.left(sep));
    QStringList headerLines = header.split("\r\n");

    if (headerLines.isEmpty()) return false;

    QStringList requestLine = headerLines.first().split(' ');
    if (requestLine.size() < 3) return false;

    method = requestLine[0];
    path = requestLine[1];

    if (method.compare("GET", Qt::CaseInsensitive) == 0) {
        body = QString();
        return true;
    }

    QRegularExpression clRe("content-length:\\s*(\\d+)",
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = clRe.match(header);
    if (!match.hasMatch()) return false;

    int contentLength = match.captured(1).toInt();
    int totalLen = sep + 4 + contentLength;

    if (data.size() < totalLen) return false;

    body = QString::fromUtf8(data.mid(sep + 4, contentLength));
    return true;
}

void McpServer::handleHealth(QTcpSocket *socket)
{
    QJsonObject health;
    health["status"] = "ok";
    health["server"] = "CandleMCP";
    health["version"] = "11.2";
    health["port"] = m_port;
    health["device_state"] = m_deviceState;
    health["sender_state"] = m_senderState;
    sendJsonResponse(socket, 200, health);
}

void McpServer::handleMcpRequest(QTcpSocket *socket, const QString &body)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        QJsonObject error = makeJsonRpcError(-1, -32700, "Parse error: " + parseError.errorString());
        sendJsonResponse(socket, 400, error);
        return;
    }

    if (!doc.isObject()) {
        sendJsonResponse(socket, 400, makeJsonRpcError(-1, -32600, "Invalid Request"));
        return;
    }

    QJsonObject request = doc.object();
    QString jsonrpc = request["jsonrpc"].toString();
    QString method = request["method"].toString();
    int id = request["id"].toInt(-1);
    QJsonObject params = request["params"].toObject();

    if (jsonrpc != "2.0") {
        sendJsonResponse(socket, 400, makeJsonRpcError(-1, -32600, "Invalid JSON-RPC version"));
        return;
    }

    QJsonObject result;

    if (method.isEmpty()) {
        handleHealth(socket);
        return;
    }

    if (method == "initialize") {
        result = handleMcpInitialize(params);
    } else if (method == "tools/list") {
        result = handleMcpToolsList(params);
    } else if (method == "tools/call") {
        QJsonObject toolResult = handleMcpToolsCall(params);
        QJsonObject envelope;

        if (toolResult.contains("content") && toolResult["content"].isArray()) {
            envelope["content"] = toolResult["content"];
            envelope["isError"] = false;
        } else {
            QJsonObject contentItem;
            contentItem["type"] = "text";
            contentItem["text"] = QString::fromUtf8(
                QJsonDocument(toolResult).toJson(QJsonDocument::Compact));
            QJsonArray content;
            content.append(contentItem);
            envelope["content"] = content;
            envelope["isError"] = toolResult.contains("error");
        }

        result = envelope;
    } else {
        result = makeJsonRpcError(id, -32601, "Method not found: " + method);
    }

    if (id >= 0) {
        sendJsonResponse(socket, 200, makeJsonRpcResponse(id, QJsonValue(result)));
    } else {
        sendJsonResponse(socket, 202, result);
    }
}

QJsonObject McpServer::handleMcpInitialize(const QJsonObject &)
{
    QJsonObject result;
    result["protocolVersion"] = "2024-11-05";
    QJsonObject serverInfo;
    serverInfo["name"] = "CandleMCP";
    serverInfo["version"] = "11.2";
    result["serverInfo"] = serverInfo;
    result["capabilities"] = QJsonObject{{"tools", QJsonObject()}};
    return result;
}

QJsonObject McpServer::handleMcpToolsList(const QJsonObject &)
{
    QJsonArray tools;

    {
        QJsonObject tool;
        tool["name"] = "send_gcode";
        tool["description"] = "Send G-code command to the connected CNC device. Supports single commands or multi-line G-code.";
        QJsonObject schema;
        schema["type"] = "object";
        QJsonObject props;
        QJsonObject cmdProp;
        cmdProp["type"] = "string";
        cmdProp["description"] = "G-code command(s) to execute";
        props["command"] = cmdProp;
        QJsonObject asyncProp;
        asyncProp["type"] = "boolean";
        asyncProp["description"] = "Execute asynchronously";
        asyncProp["default"] = true;
        props["async"] = asyncProp;
        schema["properties"] = props;
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "get_machine_status";
        tool["description"] = "Get the current state of the CNC device (disconnected, ready, running, paused, alarm).";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "get_position";
        tool["description"] = "Get the current machine coordinates in G54 work coordinate system.";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "get_feed_rate";
        tool["description"] = "Get the current feed rate settings and limits.";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "get_spindle";
        tool["description"] = "Get the spindle state including RPM and direction (CW/CCW).";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "jog";
        tool["description"] = "Move one axis by a specified distance at a given feed rate. Use for precise positioning.";
        QJsonObject schema;
        schema["type"] = "object";
        QJsonObject props;
        QJsonObject axisProp;
        axisProp["type"] = "string";
        QJsonArray axes;
        axes << "X" << "Y" << "Z";
        axisProp["enum"] = axes;
        props["axis"] = axisProp;
        QJsonObject distProp;
        distProp["type"] = "number";
        distProp["description"] = "Distance in mm (positive or negative)";
        props["distance"] = distProp;
        QJsonObject feedProp;
        feedProp["type"] = "number";
        feedProp["description"] = "Feed rate in mm/min (default 100)";
        props["feed"] = feedProp;
        schema["properties"] = props;
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "set_feed_override";
        tool["description"] = "Set the feed override percentage (0-200%). 100% = normal speed, 50% = half speed.";
        QJsonObject schema;
        schema["type"] = "object";
        QJsonObject props;
        QJsonObject pctProp;
        pctProp["type"] = "number";
        pctProp["minimum"] = 0;
        pctProp["maximum"] = 200;
        props["percent"] = pctProp;
        schema["properties"] = props;
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "set_spindle";
        tool["description"] = "Control spindle speed (RPM) and rotation direction (CW=clockwise, CCW=counter-clockwise).";
        QJsonObject schema;
        schema["type"] = "object";
        QJsonObject props;
        QJsonObject rpmProp;
        rpmProp["type"] = "number";
        rpmProp["description"] = "Spindle speed in RPM";
        props["rpm"] = rpmProp;
        QJsonObject dirProp;
        dirProp["type"] = "string";
        QJsonArray dirs;
        dirs << "CW" << "CCW";
        dirProp["enum"] = dirs;
        dirProp["default"] = "CW";
        props["direction"] = dirProp;
        schema["properties"] = props;
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "emergency_stop";
        tool["description"] = "Send emergency stop command (!) to immediately halt all motion.";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "reset_device";
        tool["description"] = "Send reset command (Ctrl+Z) to clear alarms and reset the device state.";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "list_com_ports";
        tool["description"] = "List all available serial (COM) ports on the system.";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "list_cameras";
        tool["description"] = "List camera devices visible to Candle (QCameraInfo).";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "camera_start";
        tool["description"] = "Open a camera and start the viewfinder so snapshots can be captured.";
        QJsonObject schema;
        schema["type"] = "object";
        QJsonObject props;
        QJsonObject deviceProp;
        deviceProp["type"] = "string";
        deviceProp["description"] = "Device name, description or substring; empty uses the default camera";
        props["device"] = deviceProp;
        QJsonObject widthProp;
        widthProp["type"] = "number";
        widthProp["description"] = "Requested frame width in pixels (0 = camera default)";
        props["width"] = widthProp;
        QJsonObject heightProp;
        heightProp["type"] = "number";
        heightProp["description"] = "Requested frame height in pixels (0 = camera default)";
        props["height"] = heightProp;
        schema["properties"] = props;
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "camera_stop";
        tool["description"] = "Stop and release the camera opened by camera_start.";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "camera_status";
        tool["description"] = "Report camera availability, active device, resolution and last error.";
        QJsonObject schema;
        schema["type"] = "object";
        schema["properties"] = QJsonObject();
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    {
        QJsonObject tool;
        tool["name"] = "camera_snapshot";
        tool["description"] = "Capture a still image from the camera and return it as image content (optionally save to disk).";
        QJsonObject schema;
        schema["type"] = "object";
        QJsonObject props;
        QJsonObject deviceProp;
        deviceProp["type"] = "string";
        deviceProp["description"] = "Device name, description or substring; empty uses the current/default camera";
        props["device"] = deviceProp;
        QJsonObject pathProp;
        pathProp["type"] = "string";
        pathProp["description"] = "Optional filesystem path to also save the JPEG snapshot";
        props["path"] = pathProp;
        QJsonObject widthProp;
        widthProp["type"] = "number";
        widthProp["description"] = "Requested frame width (0 = camera default)";
        props["width"] = widthProp;
        QJsonObject heightProp;
        heightProp["type"] = "number";
        heightProp["description"] = "Requested frame height (0 = camera default)";
        props["height"] = heightProp;
        schema["properties"] = props;
        tool["inputSchema"] = schema;
        tools.append(tool);
    }

    QJsonObject result;
    result["tools"] = tools;
    return result;
}

QJsonObject McpServer::handleMcpToolsCall(const QJsonObject &params)
{
    QString toolName = params["name"].toString();
    QJsonObject input = params.contains("arguments")
        ? params["arguments"].toObject()
        : params["input"].toObject();

    if (toolName == "send_gcode") return toolSendGcode(input);
    if (toolName == "get_machine_status") return toolGetMachineStatus(input);
    if (toolName == "get_position") return toolGetPosition(input);
    if (toolName == "get_feed_rate") return toolGetFeedRate(input);
    if (toolName == "get_spindle") return toolGetSpindle(input);
    if (toolName == "jog") return toolJog(input);
    if (toolName == "set_feed_override") return toolSetFeedOverride(input);
    if (toolName == "set_spindle") return toolSetSpindle(input);
    if (toolName == "emergency_stop") return toolEmergencyStop(input);
    if (toolName == "reset_device") return toolResetDevice(input);
    if (toolName == "list_com_ports") return toolListComPorts(input);
    if (toolName == "list_cameras") return toolListCameras(input);
    if (toolName == "camera_start") return toolCameraStart(input);
    if (toolName == "camera_stop") return toolCameraStop(input);
    if (toolName == "camera_status") return toolCameraStatus(input);
    if (toolName == "camera_snapshot") return toolCameraSnapshot(input);

    return makeJsonRpcError(-1, -32601, "Tool not found: " + toolName);
}

QJsonObject McpServer::toolSendGcode(const QJsonObject &params)
{
    if (!m_device) return makeJsonRpcError(-1, -32000, "Device not connected");

    QString command = params["command"].toString();
    if (command.isEmpty()) return makeJsonRpcError(-1, -32602, "command is required");

    bool async = params["async"].toBool(true);

    QMetaObject::invokeMethod(m_device, "sendCommand",
        Q_ARG(QString, command),
        Q_ARG(int, -100),
        Q_ARG(bool, async));

    QJsonObject result;
    result["success"] = true;
    result["command"] = command;
    result["async"] = async;
    return result;
}

QJsonObject McpServer::toolGetMachineStatus(const QJsonObject &)
{
    QStringList deviceNames;
    deviceNames << "unknown" << "idle" << "alarm" << "run" << "home" << "hold:0"
                << "hold:1" << "queue" << "check" << "door:0" << "door:1"
                << "door:2" << "door:3" << "jog" << "sleep";
    QStringList senderNames;
    senderNames << "transferring" << "pausing" << "paused" << "stopping"
                << "stopped" << "changing-tool";

    QJsonObject status;
    status["device_state"] = m_deviceState;
    status["sender_state"] = m_senderState;

    QJsonObject state;
    int deviceIndex = m_deviceState;
    state["name"] = (deviceIndex >= 1 && deviceIndex < deviceNames.size())
        ? deviceNames.at(deviceIndex) : "unknown";
    status["state"] = state;

    QJsonObject sender;
    sender["name"] = (m_senderState >= 0 && m_senderState < senderNames.size())
        ? senderNames.at(m_senderState) : "unknown";
    status["sender"] = sender;

    status["ready_for_jog"] = (m_deviceState == 1);
    return status;
}

QJsonObject McpServer::toolGetPosition(const QJsonObject &)
{
    QJsonObject pos;
    if (!m_device) return pos;

    Coordinates *work = m_device->property("work").value<Coordinates*>();
    Coordinates *machine = m_device->property("machine").value<Coordinates*>();
    Coordinates *probe = m_device->property("probe").value<Coordinates*>();

    if (work) {
        pos["x"] = work->x();
        pos["y"] = work->y();
        pos["z"] = work->z();
        pos["a"] = work->a();
    }
    if (machine) {
        pos["machine_x"] = machine->x();
        pos["machine_y"] = machine->y();
        pos["machine_z"] = machine->z();
    }
    if (probe) {
        pos["probe_z"] = probe->z();
    }
    return pos;
}

QJsonObject McpServer::toolGetFeedRate(const QJsonObject &)
{
    QJsonObject fr;
    fr["feed_override_percent"] = m_feedOverride;
    fr["rapid_override_percent"] = m_rapidOverride;
    fr["unit"] = "percent";
    return fr;
}

QJsonObject McpServer::toolGetSpindle(const QJsonObject &)
{
    QJsonObject sp;
    sp["rpm"] = m_spindleRpm;
    sp["direction"] = m_spindleCw ? "CW" : "CCW";
    sp["enabled"] = m_spindleEnabled;
    return sp;
}

QJsonObject McpServer::toolJog(const QJsonObject &params)
{
    if (!m_device) return makeJsonRpcError(-1, -32000, "Device not connected");
    if (m_deviceState != 1) return makeJsonRpcError(-1, -32000, "Device must be ready for jog");

    QString axis = params["axis"].toString().toUpper();
    double distance = params["distance"].toDouble();
    double feed = params["feed"].toDouble();

    if (axis.isEmpty() || (axis != "X" && axis != "Y" && axis != "Z")) {
        return makeJsonRpcError(-1, -32602, "axis is required and must be X, Y, or Z");
    }

    if (feed <= 0) feed = 100.0;

    QString cmd = QString("G91 G1 F%1 %2%3").arg(feed).arg(axis).arg(distance);

    QMetaObject::invokeMethod(m_device, "sendCommand",
        Q_ARG(QString, cmd),
        Q_ARG(int, -100),
        Q_ARG(bool, true));

    QJsonObject result;
    result["success"] = true;
    result["command"] = cmd;
    result["axis"] = axis;
    result["distance"] = distance;
    result["feed"] = feed;
    return result;
}

QJsonObject McpServer::toolSetFeedOverride(const QJsonObject &params)
{
    if (!m_device) return makeJsonRpcError(-1, -32000, "Device not connected");

    int percent = qRound(params["percent"].toDouble());
    if (percent < 10 || percent > 200) {
        return makeJsonRpcError(-1, -32602, "percent must be between 10 and 200");
    }

    int delta = percent - m_feedOverride;

    while (delta >= 10) {
        QMetaObject::invokeMethod(m_device, "sendRuntimeCommand", Q_ARG(QString, QString(QChar(0x91))));
        delta -= 10;
    }
    while (delta <= -10) {
        QMetaObject::invokeMethod(m_device, "sendRuntimeCommand", Q_ARG(QString, QString(QChar(0x90))));
        delta += 10;
    }
    while (delta > 0) {
        QMetaObject::invokeMethod(m_device, "sendRuntimeCommand", Q_ARG(QString, QString(QChar(0x93))));
        delta -= 1;
    }
    while (delta < 0) {
        QMetaObject::invokeMethod(m_device, "sendRuntimeCommand", Q_ARG(QString, QString(QChar(0x94))));
        delta += 1;
    }

    m_feedOverride = percent;

    QJsonObject result;
    result["success"] = true;
    result["feed_override_percent"] = percent;
    return result;
}

QJsonObject McpServer::toolSetSpindle(const QJsonObject &params)
{
    if (!m_device) return makeJsonRpcError(-1, -32000, "Device not connected");

    int rpm = qRound(params["rpm"].toDouble());
    QString dir = params["direction"].toString().toUpper();

    if (rpm < 0) return makeJsonRpcError(-1, -32602, "RPM must be non-negative");

    bool cw = (dir != "CCW");
    QString cmd;
    if (rpm == 0) {
        cmd = "M5";
    } else {
        cmd = QString("%1 S%2").arg(cw ? "M3" : "M4").arg(rpm);
    }

    QMetaObject::invokeMethod(m_device, "sendCommand",
        Q_ARG(QString, cmd),
        Q_ARG(int, -100),
        Q_ARG(bool, true));

    m_spindleRpm = rpm;
    m_spindleCw = cw;
    m_spindleEnabled = rpm > 0;

    QJsonObject result;
    result["success"] = true;
    result["rpm"] = rpm;
    result["direction"] = cw ? "CW" : "CCW";
    result["command"] = cmd;
    return result;
}

QJsonObject McpServer::toolEmergencyStop(const QJsonObject &)
{
    if (!m_device) return makeJsonRpcError(-1, -32000, "Device not connected");

    QMetaObject::invokeMethod(m_device, "sendCommand",
        Q_ARG(QString, "!"),
        Q_ARG(int, -100),
        Q_ARG(bool, true));

    QJsonObject result;
    result["success"] = true;
    result["action"] = "emergency_stop";
    return result;
}

QJsonObject McpServer::toolResetDevice(const QJsonObject &)
{
    if (!m_device) return makeJsonRpcError(-1, -32000, "Device not connected");

    QMetaObject::invokeMethod(m_device, "sendCommand",
        Q_ARG(QString, "\x18"),
        Q_ARG(int, -100),
        Q_ARG(bool, true));

    QJsonObject result;
    result["success"] = true;
    result["action"] = "reset";
    return result;
}

QJsonObject McpServer::toolListComPorts(const QJsonObject &)
{
    QJsonArray ports;
    QList<QSerialPortInfo> infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos) {
        QJsonObject port;
        port["portName"] = info.portName();
        port["description"] = info.description().isEmpty() ? info.portName() : info.description();
        port["manufacturer"] = info.manufacturer().isEmpty() ? "Unknown" : info.manufacturer();
        port["vendorId"] = info.vendorIdentifier();
        port["productId"] = info.productIdentifier();
        ports.append(port);
    }

    QJsonObject result;
    result["ports"] = ports;
    result["count"] = ports.size();
    return result;
}

QJsonObject McpServer::toolListCameras(const QJsonObject &)
{
    QJsonArray cameras;
    QList<QCameraInfo> infos = QCameraInfo::availableCameras();
    QCameraInfo defaultInfo = QCameraInfo::defaultCamera();

    for (const QCameraInfo &info : infos) {
        QJsonObject item;
        item["deviceName"] = info.deviceName();
        item["description"] = info.description().isEmpty() ? info.deviceName() : info.description();
        item["default"] = (!defaultInfo.isNull() && info.deviceName() == defaultInfo.deviceName());
        item["orientation"] = info.orientation();
        cameras.append(item);
    }

    QJsonObject result;
    result["cameras"] = cameras;
    result["count"] = cameras.size();
    result["activeDevice"] = m_cameraDeviceName;
    result["active"] = (m_camera && m_camera->state() == QCamera::ActiveState);
    return result;
}

QJsonObject McpServer::toolCameraStart(const QJsonObject &params)
{
    QString device = params["device"].toString();
    QSize resolution(params["width"].toInt(0), params["height"].toInt(0));
    if (resolution.width() <= 0 || resolution.height() <= 0) {
        resolution = QSize();
    }

    QString error;
    if (!ensureCamera(device, resolution, error)) {
        return makeJsonRpcError(-1, -32010, error);
    }

    QJsonObject result;
    result["success"] = true;
    result["device"] = m_cameraDeviceName;
    result["state"] = "active";
    result["resolution"] = m_cameraResolution.isValid()
        ? QString("%1x%2").arg(m_cameraResolution.width()).arg(m_cameraResolution.height())
        : QString("auto");
    return result;
}

QJsonObject McpServer::toolCameraStop(const QJsonObject &)
{
    bool wasActive = (m_camera != nullptr);
    releaseCamera();

    QJsonObject result;
    result["success"] = true;
    result["wasActive"] = wasActive;
    return result;
}

QJsonObject McpServer::toolCameraStatus(const QJsonObject &)
{
    QJsonObject result;
    result["available"] = QCameraInfo::availableCameras().count();
    result["active"] = (m_camera && m_camera->state() == QCamera::ActiveState);
    result["device"] = m_cameraDeviceName;
    result["state"] = m_camera ? int(m_camera->state()) : -1;

    if (m_camera) {
        result["errorString"] = m_camera->errorString();
        QSize actual = m_camera->viewfinderSettings().resolution();
        result["resolution"] = QString("%1x%2").arg(actual.width()).arg(actual.height());
    }
    if (!m_cameraLastError.isEmpty()) {
        result["lastError"] = m_cameraLastError;
    }
    return result;
}

QJsonObject McpServer::toolCameraSnapshot(const QJsonObject &params)
{
    QString device = params["device"].toString();
    QString targetPath = params["path"].toString();
    QSize resolution(params["width"].toInt(0), params["height"].toInt(0));
    if (resolution.width() <= 0 || resolution.height() <= 0) {
        resolution = QSize();
    }

    QString error;
    if (!ensureCamera(device, resolution, error)) {
        return makeJsonRpcError(-1, -32010, error);
    }

    if (!m_capture) {
        m_capture = new QCameraImageCapture(m_camera);
    }
    m_capture->setCaptureDestination(QCameraImageCapture::CaptureToFile);

    if (!m_capture->isReadyForCapture()) {
        return makeJsonRpcError(-1, -32011, "Camera is busy, retry in a moment");
    }

    QString tempPath = QDir::temp().filePath(
        QString("candle_mcp_snapshot_%1.jpg").arg(QDateTime::currentMSecsSinceEpoch()));

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(CAMERA_CAPTURE_TIMEOUT);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    QString savedPath;
    QString captureError;

    auto savedConnection = QObject::connect(m_capture, &QCameraImageCapture::imageSaved, &loop,
        [&savedPath, &loop](int, const QString &fileName) {
            savedPath = fileName;
            loop.quit();
        });
    auto errorConnection = QObject::connect(m_capture,
        QOverload<int, QCameraImageCapture::Error, const QString &>::of(&QCameraImageCapture::error),
        &loop,
        [&captureError, &loop](int, QCameraImageCapture::Error, const QString &message) {
            captureError = message;
            loop.quit();
        });

    int captureId = m_capture->capture(tempPath);
    if (captureId < 0) {
        QObject::disconnect(savedConnection);
        QObject::disconnect(errorConnection);
        return makeJsonRpcError(-1, -32012, "Failed to start image capture");
    }

    timer.start();
    loop.exec();
    QObject::disconnect(savedConnection);
    QObject::disconnect(errorConnection);

    if (savedPath.isEmpty()) {
        QFile::remove(tempPath);
        return makeJsonRpcError(-1, -32013,
            captureError.isEmpty() ? "Camera capture timed out" : captureError);
    }

    QFile imageFile(savedPath);
    if (!imageFile.open(QIODevice::ReadOnly)) {
        QFile::remove(tempPath);
        return makeJsonRpcError(-1, -32014, "Captured image could not be read");
    }

    QByteArray imageBytes = imageFile.readAll();
    imageFile.close();
    QFile::remove(savedPath);

    if (imageBytes.isEmpty()) {
        return makeJsonRpcError(-1, -32014, "Captured image is empty");
    }

    if (!targetPath.isEmpty()) {
        QFile output(targetPath);
        if (!output.open(QIODevice::WriteOnly)) {
            return makeJsonRpcError(-1, -32015, "Cannot write file: " + targetPath);
        }
        output.write(imageBytes);
        output.close();
    }

    QString mimeType = imageBytes.startsWith("\x89PNG") ? "image/png" : "image/jpeg";

    QJsonObject imageContent;
    imageContent["type"] = "image";
    imageContent["mimeType"] = mimeType;
    imageContent["data"] = QString::fromLatin1(imageBytes.toBase64());

    QJsonObject meta;
    meta["type"] = "text";
    meta["text"] = QString("captured from %1, %2 bytes%3")
        .arg(m_cameraDeviceName)
        .arg(imageBytes.size())
        .arg(targetPath.isEmpty() ? QString() : ", saved to " + targetPath);

    QJsonArray content;
    content.append(meta);
    content.append(imageContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

bool McpServer::ensureCamera(const QString &deviceName, const QSize &resolution, QString &error)
{
    error.clear();
    m_cameraLastError.clear();

    QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    if (cameras.isEmpty()) {
        error = "No camera devices found";
        return false;
    }

    QString wanted = deviceName.trimmed();
    bool sameDevice = m_camera
        && (wanted.isEmpty() || wanted.compare(m_cameraDeviceName, Qt::CaseInsensitive) == 0);
    bool sameResolution = m_camera
        && (!resolution.isValid() || resolution == m_cameraResolution);

    if (sameDevice && sameResolution && m_camera->state() == QCamera::ActiveState) {
        return true;
    }

    releaseCamera();

    QCameraInfo info;
    bool found = false;

    if (wanted.isEmpty()) {
        info = QCameraInfo::defaultCamera();
        found = !info.isNull();
    } else {
        for (const QCameraInfo &candidate : cameras) {
            if (candidate.deviceName().compare(wanted, Qt::CaseInsensitive) == 0
                || candidate.description().compare(wanted, Qt::CaseInsensitive) == 0
                || candidate.deviceName().contains(wanted, Qt::CaseInsensitive)
                || candidate.description().contains(wanted, Qt::CaseInsensitive)) {
                info = candidate;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        error = QString("Camera not found: %1").arg(wanted);
        return false;
    }

    m_cameraDeviceName = info.deviceName();
    m_cameraResolution = resolution;

    m_camera = new QCamera(info);
    QObject::connect(m_camera, QOverload<QCamera::Error>::of(&QCamera::error), this,
        [this](QCamera::Error code) {
            if (code != QCamera::NoError && m_camera) {
                m_cameraLastError = m_camera->errorString();
            }
        });

    if (resolution.isValid()) {
        QCameraViewfinderSettings settings = m_camera->viewfinderSettings();
        settings.setResolution(resolution);
        m_camera->setViewfinderSettings(settings);
    }

    m_camera->setCaptureMode(QCamera::CaptureStillImage);
    m_camera->start();

    if (m_camera->state() != QCamera::ActiveState) {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        timer.setInterval(CAMERA_START_TIMEOUT);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(m_camera, &QCamera::stateChanged, &loop,
            [&loop](QCamera::State state) {
                if (state == QCamera::ActiveState || state == QCamera::UnloadedState) {
                    loop.quit();
                }
            });
        timer.start();
        loop.exec();
    }

    if (m_camera->state() != QCamera::ActiveState) {
        error = m_cameraLastError.isEmpty()
            ? QString("Camera failed to start (state %1)").arg(int(m_camera->state()))
            : m_cameraLastError;
        releaseCamera();
        return false;
    }

    return true;
}

void McpServer::releaseCamera()
{
    if (m_capture) {
        m_capture->cancelCapture();
        delete m_capture;
        m_capture = nullptr;
    }

    if (m_camera) {
        m_camera->stop();
        m_camera->unload();
        delete m_camera;
        m_camera = nullptr;
    }

    m_cameraDeviceName.clear();
    m_cameraResolution = QSize();
}

void McpServer::sendJsonResponse(QTcpSocket *socket, int statusCode, const QJsonObject &body)
{
    QString statusText;
    switch (statusCode) {
        case 200: statusText = "OK"; break;
        case 201: statusText = "Created"; break;
        case 202: statusText = "Accepted"; break;
        case 400: statusText = "Bad Request"; break;
        case 404: statusText = "Not Found"; break;
        default: statusText = "Error"; break;
    }

    QString jsonBody = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray jsonBytes = jsonBody.toUtf8();
    QString response = QString(
        "HTTP/1.1 %1 %2\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Mcp-Session-Id\r\n"
        "Content-Length: %3\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).arg(statusCode).arg(statusText).arg(jsonBytes.size());

    socket->write(response.toUtf8());
    socket->write(jsonBytes);
    socket->flush();
    socket->disconnectFromHost();
}

QJsonObject McpServer::makeJsonRpcResponse(int id, const QJsonValue &result)
{
    QJsonObject response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    return response;
}

QJsonObject McpServer::makeJsonRpcError(int id, int code, const QString &message)
{
    QJsonObject error;
    error["code"] = code;
    error["message"] = message;
    QJsonObject response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = error;
    return response;
}
