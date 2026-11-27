import pickle, sys
def export(net, mu, sd, path, name):
    with open(path, 'w') as f:
        f.write('nowcast-mlp 1\n')
        f.write('name %s\n' % name)
        f.write('features %d\n' % len(mu))
        f.write(' '.join('%.9g' % v for v in mu) + '\n')
        f.write(' '.join('%.9g' % v for v in sd) + '\n')
        f.write('layers %d\n' % len(net.coefs_))
        for W, b in zip(net.coefs_, net.intercepts_):
            f.write('%d %d\n' % W.shape)
            f.write(' '.join('%.9g' % v for v in W.ravel()) + '\n')
            f.write(' '.join('%.9g' % v for v in b) + '\n')
if __name__ == '__main__':
    net, mu, sd = pickle.load(open(sys.argv[1], 'rb'))
    export(net, mu, sd, sys.argv[2], sys.argv[3])
