#
# Copyright (C) 2004-2012  See the AUTHORS file for details.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 as published
# by the Free Software Foundation.
#

import subprocess
import OpenSSL.rand
import shutil
import os.path
import distutils.dir_util
from base64 import b64encode
from urllib.parse import quote

import znc

class IrisSock(znc.Socket):
    def Init(self, port, client, path, password):
        self.client = None
        self.path = path
        self.DisableReadLine()

        self.Connect("127.0.0.1", port)

        if path == 'index':
            path = ''

        post_params = dict()
        for k, v in client.GetParams(True).items():
            for x in v:
                post_params[k] = x

        get_params = dict()
        for k, v in client.GetParams(False).items():
            for x in v:
                get_params[k] = x

        enc_get_params = []
        for k, v in get_params.items():
            enc_get_params.append(quote(k) + '=' + quote(v))

        enc_get_params = '&'.join(enc_get_params)

        if enc_get_params != '':
            enc_get_params = '?' + enc_get_params

        if client.IsPost():
            method = "POST"
        else:
            method = "GET"

        self.Write("{} /{}{} HTTP/1.1\r\n".format(method, path, enc_get_params))
        self.Write("Connection: Close\r\n")

        if client.IsPost():
            if path == 'e/n':
                post_params['nick'] = client.GetSession().GetUser().GetUserName() + "/" + post_params['network']
                post_params['password'] = password

            enc_post_params = []
            for k, v in post_params.items():
                enc_post_params.append(quote(k) + '=' + quote(v))

            post_str = "&".join(enc_post_params).encode('UTF-8')
            self.Write("Content-Length: {}\r\n".format(len(post_str)))
            self.Write("Content-Type: application/x-www-form-urlencoded\r\n")
            print("Writing post of length {}: {}".format(len(post_str), post_str))
        self.Write("\r\n")

        if client.IsPost():
            self.WriteBytes(post_str)

        self.client = self.GetModule().CreateSocket(ClientSock)
        b = znc.CZNC.Get().GetManager().SwapSockByAddr(self.client._csock, client)
        if not b:
            raise Exception("Couldn't swap client socket")

        self.client.iris = self

    def OnReadData(self, data):
        if self.client:
            self.client.WriteBytes(data)

    def OnShutdown(self):
        if self.client:
            self.client.iris = None
            self.client.Close(znc.Csock.CLT_AFTERWRITE)

class ClientSock(znc.Socket):
    def Init(self):
        self.iris = None
        self.DisableReadLine()

    def OnShutdown(self):
        if self.iris:
            self.iris.client = None
            self.iris.Close()

class webchat(znc.Module):
    description = 'Web chat'

    module_types = [znc.CModInfo.GlobalModule]

    def OnLoad(self, args, retmsg):
        self.iris = None
        self.port = None
        self.password = b64encode(OpenSSL.rand.bytes(50)).decode()

        our_port = None # find our port
        for l in znc.CZNC.Get().GetListeners():
            if znc.CListener.ACCEPT_HTTP == l.GetAcceptType():
                continue
            our_port = l.GetPort()
            our_host = l.GetBindHost()
            our_addrtype = l.GetAddrType()
            our_ssl = "true" if l.IsSSL() else "false";
        if our_port == None:
            retmsg.s = "Can't find IRC-capable listener"
            return False

        if our_host == "0.0.0.0":
            our_host = "127.0.0.1"
        elif our_host == "::":
            our_host = "::1"
        elif our_host == "":
            if our_addrtype == znc.ADDR_IPV6ONLY:
                our_host = "::1"
            else:
                our_host = "127.0.0.1"

        try:
            iris_port = int(str(args))
            self.port = iris_port
        except ValueError:
            retmsg.s = "An argument needed - the port where Iris will listen"
            return False

        rsync = subprocess.Popen(
                ["rsync", "-av", "--delete", self.GetModDataDir() + "/", self.GetSavePath()]
                )
        rsync.communicate()
        if rsync.returncode != 0:
            retmsg.s = "Couldn't copy webchat files to user directory"
            return False

        template = open(self.GetSavePath() + "/iris.conf.template", "r").read()

        new_conf = template.format(**locals())

        open(self.GetSavePath() + "/iris.conf", "w").write(new_conf)

        compiler = subprocess.Popen(
                ["python", self.GetSavePath() + "/compile.py"],
                cwd=self.GetSavePath()
                )
        compiler.communicate()
        if compiler.returncode != 0:
            retmsg.s = "Couldn't compile Iris: " + str(compiler.returncode)
            return False

        self.iris = subprocess.Popen(
                ["python", self.GetSavePath() + "/run.py"],
                cwd=self.GetSavePath()
                )

        return True

    def OnShutdown(self):
        if self.iris:
            self.iris.kill()

    def GetWebMenuTitle(self):
        return "Web Chat"

    def OnWebPreRequest(self, sock, page):
        if page == 'znc-csrf.js':
            return False

        self.CreateSocket(IrisSock, self.port, sock, page, self.password)
        return True

    def OnWebRequest(self, sock, page, tmpl):
# only znc-csrf.js can be here, and we want to print the template
        nets = sock.GetSession().GetUser().GetNetworks()
        jsnets = '['
        first = True
        for n in nets:
            if first:
                first = False
            else:
                jsnets += ', '
            jsnets += "'{}'".format(n.GetName())
        jsnets += ']'
        tmpl["networks"] = jsnets
        return True

    def OnLoginAttempt(self, auth):
        username = auth.GetUsername()
        password = auth.GetPassword()

        if (password != self.password):
            return znc.CONTINUE

        user = znc.CZNC.Get().FindUser(username)
        if not user:
            return znc.CONTINUE

        auth.AcceptLogin(user)
        return znc.HALT




















