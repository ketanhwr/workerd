// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import http from 'node:http';
import https from 'node:https';
import { strictEqual, ok, deepStrictEqual, throws } from 'node:assert';

export const checkPortsSetCorrectly = {
  test(_ctrl, env) {
    const keys = [
      'PONG_SERVER_PORT',
      'ASD_SERVER_PORT',
      'TIMEOUT_SERVER_PORT',
      'HELLO_WORLD_SERVER_PORT',
      'HEADER_VALIDATION_SERVER_PORT',
    ];
    for (const key of keys) {
      strictEqual(typeof env[key], 'string');
      ok(env[key].length > 0);
    }
  },
};

// Test is taken from Node.js: test/parallel/test-http-client-request-options.js
export const testHttpClientRequestOptions = {
  async test(_ctrl, env) {
    const headers = { foo: 'Bar' };
    const { promise, resolve } = Promise.withResolvers();
    const url = new URL(`http://localhost:${env.PONG_SERVER_PORT}/ping?q=term`);
    url.headers = headers;
    const clientReq = http.request(url);
    clientReq.on('close', resolve);
    clientReq.end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-client-res-destroyed.js
export const testHttpClientResDestroyed = {
  async test(_ctrl, env) {
    {
      const { promise, resolve } = Promise.withResolvers();
      http.get(
        {
          port: env.ASD_SERVER_PORT,
        },
        (res) => {
          strictEqual(res.destroyed, false);
          res.destroy();
          strictEqual(res.destroyed, true);
          res.on('close', resolve);
        }
      );
      await promise;
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      http.get(
        {
          port: env.ASD_SERVER_PORT,
        },
        (res) => {
          strictEqual(res.destroyed, false);
          res
            .on('close', () => {
              strictEqual(res.destroyed, true);
              resolve();
            })
            .resume();
        }
      );
      await promise;
    }
  },
};

// TODO(soon): Support this test case, if possible with the current implementation
// Test is taken from test/parallel/test-http-client-response-timeout.js
// export const testHttpClientResponseTimeout = {
//   async test(_ctrl, env) {
//     const { promise, resolve } = Promise.withResolvers();
//     const req =
//       http.get({ port: env.TIMEOUT_SERVER_PORT }, (res) => {
//         res.on('timeout', () => {
//           resolve();
//           req.destroy();
//         });
//         res.setTimeout(1);
//       });
//     await promise;
//   },
// };

// Test is taken from test/parallel/test-http-content-length.js
export const testHttpContentLength = {
  async test(_ctrl, env) {
    const expectedHeadersEndWithData = {
      connection: 'keep-alive',
      'content-length': String('hello world'.length),
    };

    const expectedHeadersEndNoData = {
      connection: 'keep-alive',
      'content-length': '0',
    };

    const { promise, resolve } = Promise.withResolvers();
    let req;

    req = http.request({
      port: env.HELLO_WORLD_SERVER_PORT,
      method: 'POST',
      path: '/end-with-data',
    });
    req.removeHeader('Date');
    req.end('hello world');
    req.on('response', function (res) {
      deepStrictEqual(res.headers, {
        ...expectedHeadersEndWithData,
        'keep-alive': 'timeout=1',
      });
      res.resume();
    });

    req = http.request({
      port: env.HELLO_WORLD_SERVER_PORT,
      method: 'POST',
      path: '/empty',
    });
    req.removeHeader('Date');
    req.end();
    req.on('response', function (res) {
      deepStrictEqual(res.headers, {
        ...expectedHeadersEndNoData,
        'keep-alive': 'timeout=1',
      });
      res.resume();
      resolve();
    });
    await promise;
  },
};

// Test is taken from test/parallel/test-http-contentLength0.js
export const testHttpContentLength0 = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const request = http.request(
      {
        port: env.HELLO_WORLD_SERVER_PORT,
        method: 'POST',
        path: '/content-length0',
      },
      (response) => {
        response.on('error', reject);
        response.resume();
        response.on('end', resolve);
      }
    );
    request.on('error', reject);
    request.end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-dont-set-default-headers-with-set-header.js
export const testHttpDontSetDefaultHeadersWithSetHeader = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const req = http.request({
      method: 'POST',
      port: env.HEADER_VALIDATION_SERVER_PORT,
      setDefaultHeaders: false,
      path: '/test-1',
    });

    req.setHeader('test', 'value');
    req.setHeader('HOST', `localhost:${env.HEADER_VALIDATION_SERVER_PORT}`);
    req.setHeader('foo', ['bar', 'baz']);
    req.setHeader('connection', 'close');
    req.on('response', resolve);
    req.on('error', reject);
    strictEqual(req.headersSent, false);
    req.end();
    await promise;
    strictEqual(req.headersSent, true);
  },
};

// Test is taken from test/parallel/test-http-dont-set-default-headers-with-setHost.js
export const testHttpDontSetDefaultHeadersWithSetHost = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    http
      .request({
        method: 'POST',
        port: env.HEADER_VALIDATION_SERVER_PORT,
        setDefaultHeaders: false,
        setHost: true,
        path: '/test-2',
      })
      .on('error', reject)
      .on('response', resolve)
      .end();
    await promise;
  },
};

// Test is taken from test/parallel/test-http-request-end-twice.js
export const testHttpRequestEndTwice = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const req = http
      .get({ port: env.HEADER_VALIDATION_SERVER_PORT }, function (res) {
        res.on('error', reject).on('end', function () {
          strictEqual(req.end(), req);
          resolve();
        });
        res.resume();
      })
      .on('error', reject);
    await promise;
  },
};

// Test is taken from test/parallel/test-http-request-host-header.js
export const testHttpRequestHostHeader = {
  async test(_ctrl, env) {
    // From RFC 7230 5.4 https://datatracker.ietf.org/doc/html/rfc7230#section-5.4
    // A server MUST respond with a 400 (Bad Request) status code to any
    // HTTP/1.1 request message that lacks a Host header field
    const { promise, resolve } = Promise.withResolvers();
    http.get(
      { port: env.HEADER_VALIDATION_SERVER_PORT, headers: [] },
      (res) => {
        strictEqual(res.statusCode, 400);
        strictEqual(res.headers.connection, 'close');
        resolve();
      }
    );
    await promise;
  },
};

// Test is taken from test/parallel/test-http-request-invalid-method-error.js
export const testHttpRequestInvalidMethodError = {
  async test() {
    throws(() => http.request({ method: '\0' }), {
      code: 'ERR_INVALID_HTTP_TOKEN',
      name: 'TypeError',
      message: 'Method must be a valid HTTP token ["\u0000"]',
    });
  },
};

export const testHttpRequestJoinAuthorizationHeaders = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    http.get(
      {
        port: env.HELLO_WORLD_SERVER_PORT,
        method: 'POST',
        headers: [
          'authorization',
          '1',
          'authorization',
          '2',
          'cookie',
          'foo',
          'cookie',
          'bar',
        ],
        joinDuplicateHeaders: true,
        path: '/join-duplicate-headers',
      },
      (res) => {
        strictEqual(res.statusCode, 200);
        strictEqual(res.headers.authorization, '3, 4');
        strictEqual(res.headers.cookie, 'foo; bar');
        resolve();
      }
    );
    await promise;
  },
};

// Test is taken from test/parallel/test-https-agent-constructor.js
export const testHttpsAgentConstructor = {
  async test() {
    ok(new https.Agent() instanceof https.Agent);
    strictEqual(typeof https.request, 'function');
    strictEqual(typeof http.get, 'function');
  },
};

// Test is taken from test/parallel/test-http-set-timeout.js
export const testHttpSetTimeout = {
  async test(_ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const request = http.get({ port: env.TIMEOUT_SERVER_PORT, path: '/' });
    request.setTimeout(100);
    request.on('error', reject);
    request.on('timeout', resolve);
    request.end();
    await promise;
  },
};

export const httpRedirectsAreNotFollowed = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const req = http.request(
      {
        port: 80,
        method: 'GET',
        protocol: 'http:',
        hostname: 'cloudflare.com',
        path: '/',
      },
      (res) => {
        strictEqual(res.statusCode, 301);
        resolve();
      }
    );
    req.end();
    await promise;
  },
};
