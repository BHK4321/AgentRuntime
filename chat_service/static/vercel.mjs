const backend = process.env.AGENTOS_BACKEND_URL?.replace(/\/$/, '');
if (!backend || !/^https:\/\/[^/]+$/.test(backend)) {
  throw new Error('Set AGENTOS_BACKEND_URL to the Render HTTPS origin in Vercel.');
}

export const config = {
  outputDirectory: '.',
  rewrites: [
    {source: '/api/:path*', destination: `${backend}/api/:path*`},
    {source: '/static/:path*', destination: '/:path*'},
  ],
};
