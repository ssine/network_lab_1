
/* FRAME kind */
#define FRAME_DATA 1
#define FRAME_ACK  2
#define FRAME_NAK  3

/*
    "b" means bit, others are in byte

    DATA Frame
    +==========+=========+=========+===============+========+
    | KIND(2b) | SEQ(3b) | ACK(3b) | DATA(240~256) | CRC(4) |
    +==========+=========+=========+===============+========+

    ACK Frame
    +==========+=========+========+
    | KIND(2b) | ACK(3b) | CRC(4) |
    +==========+=========+========+

    NAK Frame
    +==========+=========+========+
    | KIND(2b) | NAK(3b) | CRC(4) |
    +==========+=========+========+
*/


