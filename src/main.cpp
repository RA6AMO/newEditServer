#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QDebug>
#include <iostream>
#include <QList>

class Server : public QObject
{
    Q_OBJECT

public:
    explicit Server(QObject *parent = nullptr) : QObject(parent)
    {
        server = new QTcpServer(this);

        connect(server, &QTcpServer::newConnection, this, &Server::onNewConnection);

        const quint16 port = 12345;
        if (server->listen(QHostAddress::Any, port)) {
            qDebug() << "[СЕРВЕР] Сервер запущен на порту" << port;
            std::cout << "[СЕРВЕР] Сервер запущен на порту " << port << std::endl;
            std::cout << "[СЕРВЕР] Ожидание подключений..." << std::endl;
        } else {
            qDebug() << "[СЕРВЕР] Ошибка запуска сервера:" << server->errorString();
            std::cerr << "[СЕРВЕР] Ошибка запуска сервера: "
                      << server->errorString().toStdString() << std::endl;
        }
    }

private slots:
    void onNewConnection()
    {
        QTcpSocket *clientSocket = server->nextPendingConnection();
        if (!clientSocket) {
            return;
        }

        qDebug() << "[СЕРВЕР] Новое подключение от" << clientSocket->peerAddress().toString();
        std::cout << "[СЕРВЕР] Новое подключение от "
                  << clientSocket->peerAddress().toString().toStdString() << std::endl;

        clients.append(clientSocket);

        connect(clientSocket, &QTcpSocket::readyRead, this, [this, clientSocket]() {
            onClientReadyRead(clientSocket);
        });

        connect(clientSocket, &QTcpSocket::disconnected, this, [this, clientSocket]() {
            qDebug() << "[СЕРВЕР] Клиент отключился:" << clientSocket->peerAddress().toString();
            std::cout << "[СЕРВЕР] Клиент отключился: "
                      << clientSocket->peerAddress().toString().toStdString() << std::endl;
            clients.removeAll(clientSocket);
            clientSocket->deleteLater();
        });

        // Отправляем приветственное сообщение
        QString welcomeMessage = "Добро пожаловать на сервер!";
        QByteArray data = welcomeMessage.toUtf8();
        clientSocket->write(data);
        qDebug() << "[СЕРВЕР] Отправлено:" << welcomeMessage;
        std::cout << "[СЕРВЕР] Отправлено: " << welcomeMessage.toStdString() << std::endl;
    }

    void onClientReadyRead(QTcpSocket *clientSocket)
    {
        QByteArray data = clientSocket->readAll();
        QString message = QString::fromUtf8(data);
        qDebug() << "[СЕРВЕР] Получено от" << clientSocket->peerAddress().toString() << ":" << message;
        std::cout << "[СЕРВЕР] Получено от "
                  << clientSocket->peerAddress().toString().toStdString()
                  << ": " << message.toStdString() << std::endl;

        // Отправляем ответ
        QString response = "Ответ сервера на: " + message;
        QByteArray responseData = response.toUtf8();
        clientSocket->write(responseData);
        qDebug() << "[СЕРВЕР] Отправлено:" << response;
        std::cout << "[СЕРВЕР] Отправлено: " << response.toStdString() << std::endl;
    }

private:
    QTcpServer *server;
    QList<QTcpSocket*> clients;
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    Server server;

    return app.exec();
}

#include "main.moc"
