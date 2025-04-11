Telegram Bot API
================

The Telegram Bot API provides an HTTP API for creating `Telegram Bots <https://core.telegram.org/bots>`_.

If you've got any questions about bots or would like to report an issue with your bot, kindly contact us at `@BotSupport <https://t.me/BotSupport>`_ in Telegram.

   This project is a **fork** of the official `Telegram Bot API server <https://github.com/tdlib/telegram-bot-api>`_. 

   In addition to all existing features, we have added support for debugging through a Web mode. You can now use the following query parameters to specify the output format:
   
   - ``?format=web``: For web-friendly output.
   - ``?format=json``: For JSON output.
   - ``?format=txt``: For plain text output (default).

Table of Contents
-----------------
- `Installation <#installation>`_
- `Dependencies <#dependencies>`_
- `Usage <#usage>`_
- `Documentation <#documentation>`_
- `Moving a bot to a local server <#moving-a-bot-to-a-local-server>`_
- `Moving a bot from one local server to another <#moving-a-bot-from-one-local-server-to-another>`_
- `License <#license>`_
  
Installation
------------

The simplest way to build and install ``Telegram Bot API server`` is to use our `Telegram Bot API server build instructions generator <https://tdlib.github.io/telegram-bot-api/build.html>`_.
If you do that, you'll only need to choose the target operating system to receive the complete build instructions.

In general, you need to install all ``Telegram Bot API server`` dependencies_ and compile the source code using CMake:

.. code-block:: bash

   git clone --recursive https://github.com/tdlib/telegram-bot-api.git
   cd telegram-bot-api
   mkdir build
   cd build
   cmake -DCMAKE_BUILD_TYPE=Release ..
   cmake --build . --target install

..


   To install **this project**, replace ``tdlib/telegram-bot-api`` with ``Vadim-Khristenko/tlbas`` in the cloning step of the instructions. Also replace all references to ``telegram-bot-api`` with ``tlbas``.
   In general, you need to install all ``Telegram Bot API server`` dependencies_ and compile the source code using CMake:

   .. code-block:: bash

      git clone --recursive https://github.com/Vadim-Khristenko/tlbas.git
      cd tlbas
      mkdir build
      cd build
      cmake -DCMAKE_BUILD_TYPE=Release ..
      cmake --build . --target install

Dependencies
------------

To build and run ``Telegram Bot API server`` you will need:

- OpenSSL
- zlib
- C++17 compatible compiler (e.g., Clang 5.0+, GCC 7.0+, MSVC 19.1+ (Visual Studio 2017.7+), Intel C++ Compiler 19+) (build only)
- gperf (build only)
- CMake (3.10+, build only)

Usage
-----

Use ``telegram-bot-api --help`` to receive the list of all available options of the Telegram Bot API server.

The only mandatory options are ``--api-id`` and ``--api-hash``. You must obtain your own ``api_id`` and ``api_hash``
as described in `Telegram Core API <https://core.telegram.org/api/obtaining_api_id>`_ and specify them using the ``--api-id`` and ``--api-hash`` options
or the ``TELEGRAM_API_ID`` and ``TELEGRAM_API_HASH`` environment variables.

To enable Bot API features not available at ``https://api.telegram.org``, specify the option ``--local``. In the local mode the Bot API server allows to:

- Download files without a size limit.
- Upload files up to 2000 MB.
- Upload files using their local path and `the file URI scheme <https://en.wikipedia.org/wiki/File_URI_scheme>`_.
- Use an HTTP URL for the webhook.
- Use any local IP address for the webhook.
- Use any port for the webhook.
- Set *max_webhook_connections* up to 100000.
- Receive the absolute local path as a value of the *file_path* field without the need to download the file after a *getFile* request.

The Telegram Bot API server accepts only HTTP requests, so a TLS termination proxy needs to be used to handle remote HTTPS requests.

By default the Telegram Bot API server is launched on the port 8081, which can be changed using the option ``--http-port``.

Documentation
-------------

- See `Bots: An introduction for developers <https://core.telegram.org/bots>`_ for a brief description of Telegram Bots and their features.

- See the `Telegram Bot API documentation <https://core.telegram.org/bots/api>`_ for a description of the Bot API interface and a complete list of available classes, methods and updates.

- See the `Telegram Bot API server build instructions generator <https://tdlib.github.io/telegram-bot-api/build.html>`_ for detailed instructions on how to build the Telegram Bot API server.

- Subscribe to `@BotNews <https://t.me/botnews>`_ to be the first to know about the latest updates and join the discussion in `@BotTalk <https://t.me/bottalk>`_.

Moving a bot to a local server
------------------------------

To guarantee that your bot will receive all updates, you must deregister it with the ``https://api.telegram.org`` server by calling the method `logOut <https://core.telegram.org/bots/api#logout>`_.
After the bot is logged out, you can replace the address to which the bot sends requests with the address of your local server and use it in the usual way.
If the server is launched in ``--local`` mode, make sure that the bot can correctly handle absolute file paths in response to ``getFile`` requests.

Moving a bot from one local server to another
---------------------------------------------

If the bot is logged in on more than one server simultaneously, there is no guarantee that it will receive all updates.
To move a bot from one local server to another you can use the method `logOut <https://core.telegram.org/bots/api#logout>`_ to log out on the old server before switching to the new one.

If you want to avoid losing updates between logging out on the old server and launching on the new server, you can remove the bot's webhook using the method
`deleteWebhook <https://core.telegram.org/bots/api#deletewebhook>`_, then use the method `close <https://core.telegram.org/bots/api#close>`_ to close the bot instance.
After the instance is closed, locate the bot's subdirectory in the working directory of the old server by the bot's user ID, move the subdirectory to the working directory of the new server
and continue sending requests to the new server as usual.

License
-------

`Telegram Bot API server` source code is licensed under the terms of the Boost Software License. See `LICENSE_1_0.txt <http://www.boost.org/LICENSE_1_0.txt>`_ for more information.
