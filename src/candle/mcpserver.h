// This file is a part of "Candle" application.
// Copyright 2015-2025 Hayrullin Denis Ravilevich

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QMutex>
#include <QSerialPortInfo>
#include <QSize>

class QCamera;
class QCameraImageCapture;

class McpServer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int port READ port WRITE setPort NOTIFY portChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)

public:
    explicit McpServer(QObject *parent = nullptr);
    ~McpServer();

    int port() const { return m_port; }
    void setPort(int port);
    bool isRunning() const { return m_server && m_server->isListening(); }

    void setDevice(QObject *device);
    void setState(int deviceState, int senderState);
    void setFeedOverride(int feedPercent, int rapidPercent);
    void setSpindleState(int rpm, bool cw, bool enabled);

    bool start();
    void stop();

signals:
    void portChanged(int port);
    void runningChanged(bool running);
    void message(const QString &level, const QString &message);

private slots:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();

private:
    bool parseHttpRequest(QTcpSocket *socket, const QByteArray &data,
                          QString &method, QString &path, QString &body);

    void handleHealth(QTcpSocket *socket);
    void handleMcpRequest(QTcpSocket *socket, const QString &body);

    QJsonObject handleMcpInitialize(const QJsonObject &params);
    QJsonObject handleMcpToolsList(const QJsonObject &params);
    QJsonObject handleMcpToolsCall(const QJsonObject &params);

    QJsonObject toolSendGcode(const QJsonObject &params);
    QJsonObject toolGetMachineStatus(const QJsonObject &params);
    QJsonObject toolGetPosition(const QJsonObject &params);
    QJsonObject toolGetFeedRate(const QJsonObject &params);
    QJsonObject toolGetSpindle(const QJsonObject &params);
    QJsonObject toolJog(const QJsonObject &params);
    QJsonObject toolSetFeedOverride(const QJsonObject &params);
    QJsonObject toolSetSpindle(const QJsonObject &params);
    QJsonObject toolEmergencyStop(const QJsonObject &params);
    QJsonObject toolResetDevice(const QJsonObject &params);
    QJsonObject toolListComPorts(const QJsonObject &params);
    QJsonObject toolListCameras(const QJsonObject &params);
    QJsonObject toolCameraStart(const QJsonObject &params);
    QJsonObject toolCameraStop(const QJsonObject &params);
    QJsonObject toolCameraStatus(const QJsonObject &params);
    QJsonObject toolCameraSnapshot(const QJsonObject &params);

    bool ensureCamera(const QString &deviceName, const QSize &resolution, QString &error);
    void releaseCamera();

    void sendJsonResponse(QTcpSocket *socket, int statusCode, const QJsonObject &body);
    QJsonObject makeJsonRpcResponse(int id, const QJsonValue &result);
    QJsonObject makeJsonRpcError(int id, int code, const QString &message);

    QTcpServer *m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QObject *m_device;
    int m_deviceState;
    int m_senderState;
    int m_port;
    int m_feedOverride;
    int m_rapidOverride;
    int m_spindleRpm;
    bool m_spindleCw;
    bool m_spindleEnabled;
    QMutex m_mutex;

    QCamera *m_camera;
    QCameraImageCapture *m_capture;
    QString m_cameraDeviceName;
    QSize m_cameraResolution;
    QString m_cameraLastError;
};

#endif // MCP_SERVER_H
