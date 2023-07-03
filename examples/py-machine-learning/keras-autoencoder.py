#!/usr/bin/env python3

import base64
import csv
import matplotlib.pyplot as plt
import numpy as np
import os
import pandas as pd
import tensorflow as tf
import sys

from tensorflow.keras import layers, preprocessing
from tensorflow.keras.layers import Embedding, Input, Dense
from tensorflow.keras.models import Model, Sequential
from tensorflow.keras.utils import plot_model

sys.path.append(os.path.dirname(sys.argv[0]) + '/../../dependencies')
sys.path.append(os.path.dirname(sys.argv[0]) + '/../share/nDPId')
sys.path.append(os.path.dirname(sys.argv[0]))
sys.path.append(sys.base_prefix + '/share/nDPId')
import nDPIsrvd
from nDPIsrvd import nDPIsrvdSocket, TermColor

input_size = nDPIsrvd.nDPId_PACKETS_PLEN_MAX
training_size = 500
batch_size = 100

def generate_autoencoder():
    input_i = Input(shape=())
    input_i = Embedding(input_dim=input_size, output_dim=input_size, mask_zero=True)(input_i)
    encoded_h1 = Dense(1024, activation='relu', name='input_i')(input_i)
    encoded_h2 = Dense(512, activation='relu', name='encoded_h1')(encoded_h1)
    encoded_h3 = Dense(128, activation='relu', name='encoded_h2')(encoded_h2)
    encoded_h4 = Dense(64, activation='relu', name='encoded_h3')(encoded_h3)
    encoded_h5 = Dense(32, activation='relu', name='encoded_h4')(encoded_h4)
    latent = Dense(2, activation='relu', name='encoded_h5')(encoded_h5)
    decoder_h1 = Dense(32, activation='relu', name='latent')(latent)
    decoder_h2 = Dense(64, activation='relu', name='decoder_h1')(decoder_h1)
    decoder_h3 = Dense(128, activation='relu', name='decoder_h2')(decoder_h2)
    decoder_h4 = Dense(512, activation='relu', name='decoder_h3')(decoder_h3)
    decoder_h5 = Dense(1024, activation='relu', name='decoder_h4')(decoder_h4)
    return input_i, Model(input_i, Dense(input_size, activation='sigmoid', name='decoder_h5')(decoder_h5))

def compile_autoencoder():
    inp, autoencoder = generate_autoencoder()
    autoencoder.compile(loss='mse', optimizer='adam', metrics=[tf.keras.metrics.Accuracy()])
    return inp, autoencoder

def onJsonLineRecvd(json_dict, instance, current_flow, global_user_data):
    if 'packet_event_name' not in json_dict:
        return True

    if json_dict['packet_event_name'] != 'packet' and \
        json_dict['packet_event_name'] != 'packet-flow':
        return True

    _, padded_pkts = global_user_data
    buf = base64.b64decode(json_dict['pkt'], validate=True)

    # Generate decimal byte buffer with valus from 0-255
    int_buf = []
    for v in buf:
        int_buf.append(int(v))

    mat = np.array([int_buf])

    # Normalize the values
    mat = mat.astype('float32') / 255.

    # Mean removal
    matmean = np.mean(mat, axis=0)
    mat -= matmean

    # Pad resulting matrice
    buf = preprocessing.sequence.pad_sequences(mat, padding="post", maxlen=input_size, truncating='post')
    padded_pkts.append(buf[0])

    sys.stdout.write('.')
    sys.stdout.flush()
    if (len(padded_pkts) % training_size == 0):
        print('\nGot {} packets, training..'.format(len(padded_pkts)))
        tmp = np.array(padded_pkts)
        history = autoencoder.fit(
                                  tmp, tmp, epochs=10, batch_size=batch_size,
                                  validation_split=0.2,
                                  shuffle=True
                                 )
        padded_pkts.clear()

        #plot_model(autoencoder, show_shapes=True, show_layer_names=True)
        #plt.plot(history.history['loss'])
        #plt.plot(history.history['val_loss'])
        #plt.title('model loss')
        #plt.xlabel('loss')
        #plt.ylabel('val_loss')
        #plt.legend(['loss', 'val_loss'], loc='upper left')
        #plt.show()

    return True

if __name__ == '__main__':
    sys.stderr.write('\b\n***************\n')
    sys.stderr.write('*** WARNING ***\n')
    sys.stderr.write('***************\n')
    sys.stderr.write('\nThis is an unmature Autoencoder example.\n')
    sys.stderr.write('Please do not rely on any of it\'s output!\n\n')

    argparser = nDPIsrvd.defaultArgumentParser()
    args = argparser.parse_args()
    address = nDPIsrvd.validateAddress(args)

    sys.stderr.write('Recv buffer size: {}\n'.format(nDPIsrvd.NETWORK_BUFFER_MAX_SIZE))
    sys.stderr.write('Connecting to {} ..\n'.format(address[0]+':'+str(address[1]) if type(address) is tuple else address))

    _, autoencoder = compile_autoencoder()

    nsock = nDPIsrvdSocket()
    nsock.connect(address)
    try:
        padded_pkts = list()
        nsock.loop(onJsonLineRecvd, None, (autoencoder, padded_pkts))
    except nDPIsrvd.SocketConnectionBroken as err:
        sys.stderr.write('\n{}\n'.format(err))
    except KeyboardInterrupt:
        print()