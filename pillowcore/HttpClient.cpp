#include "HttpClient.h"
#include "ByteArrayHelpers.h"
#include <QtCore/QIODevice>
#include <QtCore/QUrl>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkCookie>

namespace Pillow
{
	namespace HttpClientTokens
	{
		const QByteArray getMethodToken("GET");
		const QByteArray headMethodToken("HEAD");
		const QByteArray postMethodToken("POST");
		const QByteArray putMethodToken("PUT");
		const QByteArray deleteMethodToken("DELETE");
		const QByteArray crlfToken("\r\n");
		const QByteArray colonSpaceToken(": ");
		const QByteArray httpOneOneCrlfToken(" HTTP/1.1\r\n");
		const QByteArray contentLengthColonSpaceToken("Content-Length: ");
		const Pillow::HttpHeader acceptHeader("Accept", "*");
	}
}

//
// Pillow::HttpRequestWriter
//

Pillow::HttpRequestWriter::HttpRequestWriter(QObject *parent)
	: QObject(parent), _device(0)
{
}

void Pillow::HttpRequestWriter::get(const QByteArray &path, const Pillow::HttpHeaderCollection &headers)
{
	write(Pillow::HttpClientTokens::getMethodToken, path, headers);
}

void Pillow::HttpRequestWriter::head(const QByteArray &path, const Pillow::HttpHeaderCollection &headers)
{
	write(Pillow::HttpClientTokens::headMethodToken, path, headers);
}

void Pillow::HttpRequestWriter::post(const QByteArray &path, const Pillow::HttpHeaderCollection &headers, const QByteArray &data)
{
	write(Pillow::HttpClientTokens::postMethodToken, path, headers, data);
}

void Pillow::HttpRequestWriter::put(const QByteArray &path, const Pillow::HttpHeaderCollection &headers, const QByteArray &data)
{
	write(Pillow::HttpClientTokens::putMethodToken, path, headers, data);
}

void Pillow::HttpRequestWriter::deleteResource(const QByteArray &path, const Pillow::HttpHeaderCollection &headers)
{
	write(Pillow::HttpClientTokens::deleteMethodToken, path, headers);
}

void Pillow::HttpRequestWriter::write(const QByteArray &method, const QByteArray &path, const Pillow::HttpHeaderCollection &headers, const QByteArray &data)
{
	if (_device == 0)
	{
		qWarning() << "Pillow::HttpRequestWriter::write: called while device is not set. Not proceeding.";
		return;
	}

	if (_builder.capacity() < 8192)
		_builder.reserve(8192);

	_builder.append(method).append(' ').append(path).append(Pillow::HttpClientTokens::httpOneOneCrlfToken);

	for (const Pillow::HttpHeader *h = headers.constBegin(), *hE = headers.constEnd(); h < hE; ++h)
		_builder.append(h->first).append(Pillow::HttpClientTokens::colonSpaceToken).append(h->second).append(Pillow::HttpClientTokens::crlfToken);

	if (!data.isEmpty())
	{
		_builder.append(Pillow::HttpClientTokens::contentLengthColonSpaceToken);
		Pillow::ByteArrayHelpers::appendNumber<int, 10>(_builder, data.size());
		_builder.append(Pillow::HttpClientTokens::crlfToken);
	}

	_builder.append(Pillow::HttpClientTokens::crlfToken);

	if (data.isEmpty())
	{
		_device->write(_builder);
	}
	else
	{
		if (data.size() < 4096)
		{
			_builder.append(data);
			_device->write(_builder);
		}
		else
		{
			_device->write(_builder);
			_device->write(data);
		}
	}

	if (_builder.size() > 16384)
		_builder.clear();
	else
		_builder.data_ptr()->size = 0;
}

void Pillow::HttpRequestWriter::setDevice(QIODevice *device)
{
	if (_device == device) return;
	_device = device;
}

//
// Pillow::HttpResponseParser
//

Pillow::HttpResponseParser::HttpResponseParser()
	: _lastWasValue(false)
{
	parser.data = this;
	parser_settings.on_message_begin = parser_on_message_begin;
	parser_settings.on_header_field = parser_on_header_field;
	parser_settings.on_header_value = parser_on_header_value;
	parser_settings.on_headers_complete = parser_on_headers_complete;
	parser_settings.on_body = parser_on_body;
	parser_settings.on_message_complete = parser_on_message_complete;
	clear();
}

int Pillow::HttpResponseParser::inject(const char *data, int length)
{
	size_t consumed = http_parser_execute(&parser, &parser_settings, data, length);
	if (parser.http_errno == HPE_PAUSED) parser.http_errno = HPE_OK; // Unpause the parser that got paused upon completing the message.
	return consumed;
}

void Pillow::HttpResponseParser::injectEof()
{
	http_parser_execute(&parser, &parser_settings, 0, 0);
	if (parser.http_errno == HPE_PAUSED) parser.http_errno = HPE_OK; // Unpause the parser that got paused upon completing the message.
}

void Pillow::HttpResponseParser::clear()
{
	http_parser_init(&parser, HTTP_RESPONSE);
	while (!_headers.isEmpty()) _headers.pop_back();
	_content.clear();
}

QByteArray Pillow::HttpResponseParser::errorString() const
{
	const char* desc = http_errno_description(HTTP_PARSER_ERRNO(&parser));
	return QByteArray::fromRawData(desc, qstrlen(desc));
}

void Pillow::HttpResponseParser::messageBegin()
{
	while (!_headers.isEmpty()) _headers.pop_back();
	_content.clear();
	_lastWasValue = false;
}

void Pillow::HttpResponseParser::headersComplete()
{
}

void Pillow::HttpResponseParser::messageContent(const char *data, int length)
{
	_content.append(data, length);
}

void Pillow::HttpResponseParser::messageComplete()
{
}

int Pillow::HttpResponseParser::parser_on_message_begin(http_parser *parser)
{
	Pillow::HttpResponseParser* self = reinterpret_cast<Pillow::HttpResponseParser*>(parser->data);
	self->messageBegin();
	return 0;
}

int Pillow::HttpResponseParser::parser_on_header_field(http_parser *parser, const char *at, size_t length)
{
	Pillow::HttpResponseParser* self = reinterpret_cast<Pillow::HttpResponseParser*>(parser->data);
	self->pushHeader();
	self->_field.append(at, length);
	return 0;
}

int Pillow::HttpResponseParser::parser_on_header_value(http_parser *parser, const char *at, size_t length)
{
	Pillow::HttpResponseParser* self = reinterpret_cast<Pillow::HttpResponseParser*>(parser->data);
	self->_value.append(at, length);
	self->_lastWasValue = true;
	return 0;
}

int Pillow::HttpResponseParser::parser_on_headers_complete(http_parser *parser)
{
	Pillow::HttpResponseParser* self = reinterpret_cast<Pillow::HttpResponseParser*>(parser->data);
	self->pushHeader();
	self->headersComplete();
	return 0;
}

int Pillow::HttpResponseParser::parser_on_body(http_parser *parser, const char *at, size_t length)
{
	Pillow::HttpResponseParser* self = reinterpret_cast<Pillow::HttpResponseParser*>(parser->data);
	self->messageContent(at, static_cast<int>(length));
	return 0;
}

int Pillow::HttpResponseParser::parser_on_message_complete(http_parser *parser)
{
	Pillow::HttpResponseParser* self = reinterpret_cast<Pillow::HttpResponseParser*>(parser->data);
	self->messageComplete();

	// Pause the parser to avoid parsing the next message (if there is another one in the buffer).
	http_parser_pause(parser, 1);

	return 0;
}

inline void Pillow::HttpResponseParser::pushHeader()
{
	if (_lastWasValue)
	{
		_headers << Pillow::HttpHeader(_field, _value);
		_field.clear();
		_value.clear();
		_lastWasValue = false;
	}
}

//
// Pillow::HttpClient
//

Pillow::HttpClient::HttpClient(QObject *parent)
	: QObject(parent), _responsePending(false), _error(NoError)
{
	_device = new QTcpSocket(this);
	connect(_device, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(device_error(QAbstractSocket::SocketError)));
	connect(_device, SIGNAL(connected()), this, SLOT(device_connected()));
	connect(_device, SIGNAL(readyRead()), this, SLOT(device_readyRead()));
	_requestWriter.setDevice(_device);

	_baseRequestHeaders << Pillow::HttpClientTokens::acceptHeader;
}

bool Pillow::HttpClient::responsePending() const
{
	return _responsePending;
}

Pillow::HttpClient::Error Pillow::HttpClient::error() const
{
	return _error;
}

QByteArray Pillow::HttpClient::consumeContent()
{
	QByteArray c = _content;
	_content = QByteArray();
	return c;
}

void Pillow::HttpClient::get(const QUrl &url, const Pillow::HttpHeaderCollection &headers)
{
	request(Pillow::HttpClientTokens::getMethodToken, url, headers);
}

void Pillow::HttpClient::head(const QUrl &url, const Pillow::HttpHeaderCollection &headers)
{
	request(Pillow::HttpClientTokens::headMethodToken, url, headers);
}

void Pillow::HttpClient::post(const QUrl &url, const Pillow::HttpHeaderCollection &headers, const QByteArray &data)
{
	request(Pillow::HttpClientTokens::postMethodToken, url, headers, data);
}

void Pillow::HttpClient::put(const QUrl &url, const Pillow::HttpHeaderCollection &headers, const QByteArray &data)
{
	request(Pillow::HttpClientTokens::putMethodToken, url, headers, data);
}

void Pillow::HttpClient::deleteResource(const QUrl &url, const Pillow::HttpHeaderCollection &headers)
{
	request(Pillow::HttpClientTokens::deleteMethodToken, url, headers);
}

void Pillow::HttpClient::request(const QByteArray &method, const QUrl &url, const Pillow::HttpHeaderCollection &headers, const QByteArray &data)
{
	if (_responsePending)
	{
		qWarning() << "Pillow::HttpClient::request: cannot send new request while another one is under way. Request pipelining is not supported.";
		return;
	}

	// We can reuse an active connection if the request is for the same host and port, so make note of those parameters before they are overwritten.
	const QString previousHost = _request.url.host();
	const int previousPort = _request.url.port();

	Pillow::HttpClientRequest newRequest;
	newRequest.method = method;
	newRequest.url = url;
	newRequest.headers = headers;
	newRequest.data = data;

	_request = newRequest;
	_responsePending = true;
	_error = NoError;
	clear();

	if (_device->state() == QAbstractSocket::ConnectedState && url.host() == previousHost && url.port() == previousPort)
		sendRequest();
	else
	{
		if (_device->state() != QAbstractSocket::UnconnectedState)
			_device->disconnectFromHost();
		_device->connectToHost(url.host(), url.port());
	}
}

void Pillow::HttpClient::abort()
{
	if (!_responsePending)
	{
		qWarning("Pillow::HttpClient::abort(): called while not running.");
		return;
	}
	if (_device) _device->close();
	_error = AbortedError;
	_responsePending = false;
	emit finished();
}

void Pillow::HttpClient::device_error(QAbstractSocket::SocketError error)
{
	if (!_responsePending)
	{
		// Errors that happen while we are not waiting for a response are ok. We'll try to
		// recover on the next time we get a request.
		return;
	}

	switch (error)
	{
	case QAbstractSocket::RemoteHostClosedError:
		_error = RemoteHostClosedError;
		break;
	default:
		_error = NetworkError;
	}

	_responsePending = false;
	emit finished();
}

void Pillow::HttpClient::device_connected()
{
	sendRequest();
}

void Pillow::HttpClient::device_readyRead()
{
	if (!responsePending())
	{
		// Not supposed to be receiving data at this point. Just
		// ignore it and close the connection.
		_device->close();
		return;
	}

	qint64 bytesAvailable = _device->bytesAvailable();
	if (bytesAvailable == 0) return;

	if (_buffer.capacity() < _buffer.size() + bytesAvailable)
		_buffer.reserve(_buffer.size() + bytesAvailable);

	qint64 bytesRead = _device->read(_buffer.data() + _buffer.size(), bytesAvailable);
	_buffer.data_ptr()->size += bytesRead;

	int consumed = inject(_buffer);

	if (!hasError() && consumed < _buffer.size() && responsePending())
	{
		// We had multiple responses in the buffer?
		// It was a 100 Continue since we are still response pending.
		consumed += inject(_buffer.constData() + consumed, _buffer.size() - consumed);
	}

	if (consumed < _buffer.size() && !Pillow::HttpResponseParser::hasError())
		qDebug() << "Pillow::HttpClient::device_readyRead(): not all request data was consumed.";

	// Reuse the read buffer if it is not overly large.
	if (_buffer.capacity() > 128 * 1024)
		_buffer.clear();
	else
		_buffer.data_ptr()->size = 0;

	if (Pillow::HttpResponseParser::hasError())
	{
		_error = ResponseInvalidError;
		_device->close();
		_responsePending = false;
		emit finished();
	}
}

void Pillow::HttpClient::sendRequest()
{
	if (!responsePending()) return;

	QByteArray uri = _request.url.encodedPath();
	const QByteArray query = _request.url.encodedQuery();
	if (!query.isEmpty()) uri.append('?').append(query);

	Pillow::HttpHeaderCollection headers = _baseRequestHeaders;
	if (!_request.headers.isEmpty())
	{
		headers.reserve(headers.size() + _request.headers.size());
		for (int i = 0, iE = _request.headers.size(); i < iE; ++i)
			headers.append(_request.headers.at(i));
	}

	_requestWriter.write(_request.method, uri, headers, _request.data);
}

void Pillow::HttpClient::messageBegin()
{
	Pillow::HttpResponseParser::messageBegin();
}

void Pillow::HttpClient::headersComplete()
{
	Pillow::HttpResponseParser::headersComplete();
	emit headersCompleted();
}

void Pillow::HttpClient::messageContent(const char *data, int length)
{
	Pillow::HttpResponseParser::messageContent(data, length);
	emit contentReadyRead();
}

void Pillow::HttpClient::messageComplete()
{
	if (statusCode() != 100) // Ignore 100 Continue responses.
	{
		Pillow::HttpResponseParser::messageComplete();
		_responsePending = false;
		emit finished();
	}
}

//
// Pillow::NetworkReply
//

namespace Pillow
{
	class NetworkReply : public QNetworkReply
	{
		Q_OBJECT

	public:
		NetworkReply(Pillow::HttpClient *client, const QNetworkRequest& request)
			:_client(client), _contentPos(0)
		{
			connect(client, SIGNAL(headersCompleted()), this, SLOT(client_headersCompleted()));
			connect(client, SIGNAL(contentReadyRead()), this, SLOT(client_contentReadyRead()));
			connect(client, SIGNAL(finished()), this, SLOT(client_finished()));

			setRequest(request);

			// TODO: Authentication is not supported for now.
		}

	public:
		void abort()
		{
			if (_client) _client->abort();
		}

	private slots:
		void client_headersCompleted()
		{
			QList<QNetworkCookie> cookies;

			setAttribute(QNetworkRequest::HttpStatusCodeAttribute, _client->statusCode());

			// Setting the reason phrase is not done as the http_parser does not give us access
			// to the real reason phrase returned by the server.
			//
			// setAttribute(QNetworkRequest::HttpReasonPhraseAttribute, ...);

			foreach (const Pillow::HttpHeader &header, _client->headers())
			{
				setRawHeader(header.first, header.second);

				if (Pillow::ByteArrayHelpers::asciiEqualsCaseInsensitive(header.first, Pillow::LowerCaseToken("set-cookie")))
					cookies += QNetworkCookie::parseCookies(header.second);
				else if (Pillow::ByteArrayHelpers::asciiEqualsCaseInsensitive(header.first, Pillow::LowerCaseToken("location")))
					setAttribute(QNetworkRequest::RedirectionTargetAttribute, QUrl(header.second));
			}

			if (!cookies.isEmpty())
				setHeader(QNetworkRequest::SetCookieHeader, QVariant::fromValue(cookies));

			QNetworkAccessManager *nam = manager();
			if (nam)
			{
				QNetworkCookieJar *jar = nam->cookieJar();
				if (jar)
				{
					QUrl url = request().url();
					url.setPath("/");
					jar->setCookiesFromUrl(cookies, url);
				}
			}

			open(QIODevice::ReadOnly);

			emit metaDataChanged();
		}

		void client_contentReadyRead()
		{
			_content.append(_client->consumeContent());
			 emit readyRead();
		}

		void client_finished()
		{
			disconnect(_client, 0, this, 0);

			if (_client->error() != Pillow::HttpClient::NoError)
			{
				QNetworkReply::NetworkError error = QNetworkReply::UnknownNetworkError;

				switch (_client->error())
				{
				case Pillow::HttpClient::NoError: error = QNetworkReply::NoError; break;
				case Pillow::HttpClient::NetworkError: error = QNetworkReply::UnknownNetworkError; break;
				case Pillow::HttpClient::ResponseInvalidError: error = QNetworkReply::ProtocolUnknownError; break;
				case Pillow::HttpClient::RemoteHostClosedError: error = QNetworkReply::RemoteHostClosedError; break;
				case Pillow::HttpClient::AbortedError: error = QNetworkReply::OperationCanceledError; break;
				}

				emit this->error(error);
			}

			_client = 0;
			setFinished(true);
			emit finished();
		}

	protected:
		qint64 readData(char *data, qint64 maxSize)
		{
			if (_contentPos >= _content.size()) return -1;
			qint64 bytesRead = qMin(maxSize, static_cast<qint64>(_content.size() - _contentPos));
			memcpy(data, _content.constData() + _contentPos, bytesRead);
			_contentPos += bytesRead;
			return bytesRead;
		}

	private:
		Pillow::HttpClient *_client;
		QByteArray _content;
		int _contentPos;
	};
}

//
// Pillow::NetworkAccessManager
//

Pillow::NetworkAccessManager::NetworkAccessManager(QObject *parent)
	: QNetworkAccessManager(parent)
{
}


QNetworkReply *Pillow::NetworkAccessManager::createRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData)
{
	if (request.url().scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) != 0)
	{
		// Use base implementation for unsupported schemes.
		return QNetworkAccessManager::createRequest(op, request, outgoingData);
	}

	UrlClientsMap::Iterator it = _urlToClientsMap.find(request.url().authority());
	Pillow::HttpClient *client = 0;
	if (it != _urlToClientsMap.end())
	{
		_urlToClientsMap.erase(it);
		client = it.value();
	}
	if (client == 0)
	{
		client = new Pillow::HttpClient(this);
		_clients << client;
	}

	Pillow::NetworkReply *reply = new Pillow::NetworkReply(client, request);

	Pillow::HttpHeaderCollection headers;
	foreach (const QByteArray &headerName, request.rawHeaderList())
		headers << Pillow::HttpHeader(headerName, request.rawHeader(headerName));

	QNetworkCookieJar *jar = cookieJar();
	if (jar)
	{
		const QList<QNetworkCookie> cookies = jar->cookiesForUrl(request.url());
		QByteArray cookieHeaderValue;
		for (int i = 0, iE = cookies.size(); i < iE; ++i)
		{
			if (i > 0) cookieHeaderValue.append("; ");
			cookieHeaderValue.append(cookies.at(i).toRawForm(QNetworkCookie::NameAndValueOnly));
		}
		if (!cookieHeaderValue.isEmpty())
			headers << Pillow::HttpHeader("Cookie", cookieHeaderValue);
	}

	switch (op)
	{
	case QNetworkAccessManager::HeadOperation:
		client->head(request.url(), headers);
		break;
	case QNetworkAccessManager::GetOperation:
		client->get(request.url(), headers);
		break;
	case QNetworkAccessManager::PutOperation:
		client->put(request.url(), headers, outgoingData ? outgoingData->readAll() : QByteArray());
		break;
	case QNetworkAccessManager::PostOperation:
		client->post(request.url(), headers, outgoingData ? outgoingData->readAll() : QByteArray());
		break;
	case QNetworkAccessManager::DeleteOperation:
		client->deleteResource(request.url(), headers);
		break;
	case QNetworkAccessManager::CustomOperation:
		client->request(request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray(), request.url(), headers, outgoingData ? outgoingData->readAll() : QByteArray());
		break;

	case QNetworkAccessManager::UnknownOperation:
		break;
	}

	return reply;
}

#include "HttpClient.moc"