import socket
import time
import pygame

UDP_IP = "100.91.38.52"
UDP_PORT = 12345

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

keys = {'w': False, 'a': False, 'd': False, 's': False}

pygame.init()
screen = pygame.display.set_mode((350, 200))
pygame.display.set_caption("Rover Control Pad")

running = True
try:
    while running:
        heading = 0.0
        angle = 0.0
        
        # Avoid windows Not Responding
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

        keys = pygame.key.get_pressed()
        if keys[pygame.K_w]: heading = 0.8
        if keys[pygame.K_s]: heading = 0.0
        if keys[pygame.K_a]: angle = -0.4
        if keys[pygame.K_d]: angle = 0.4
        
        msg = f"{heading},{angle}"
                
        sock.sendto(msg.encode(), (UDP_IP, UDP_PORT))
        
        #20hz
        time.sleep(0.05)

except KeyboardInterrupt:
    pass
finally:
    sock.sendto(b"0.0,0.0", (UDP_IP, UDP_PORT))
    pygame.quit()
