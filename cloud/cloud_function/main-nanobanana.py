# This script will convert the image using gemini pro 3 in the cloud and 
# is being deprecated to not have to continually use cloud resources 

import os
#Only in macOS environments
os.environ.setdefault("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES")

import base64
import functions_framework
import json
import random
import io
from google.cloud import storage
from google.cloud import pubsub_v1
from google.cloud import secretmanager
import vertexai
from vertexai.generative_models import GenerativeModel, Part
from vertexai.preview.vision_models import ImageGenerationModel, Image as VertexImage
from google import genai 
from PIL import Image as PILImage


# Configuration
PROJECT_ID = "everframe-479214"

os.environ["GOOGLE_CLOUD_PROJECT"] = PROJECT_ID
REGION = os.environ.get("REGION", "us-central1")
BUCKET_NAME = "everframe-photo-bucket"
TOPIC_ID = "scheduler-event"

# Initialize Clients
storage_client = storage.Client()
publisher = pubsub_v1.PublisherClient()
secret_client = secretmanager.SecretManagerServiceClient()

def get_secret(secret_id, version_id="latest"):
    """
    Helper to retrieve secrets from Secret Manager.
    Useful for API keys, database passwords, etc.
    """
    try:
        name = f"projects/{PROJECT_ID}/secrets/{secret_id}/versions/{version_id}"
        response = secret_client.access_secret_version(request={"name": name})
        return response.payload.data.decode("UTF-8")
    except Exception as e:
        print(f"Error fetching secret {secret_id}: {e}")
        return None

PROMPTS = [
    "watercolor painting",
    "black and white charcoal sketch",
    "vaporwave digital art",
    "oil painting with thick impasto",
    "minimalist vector art",
    "vintage polaroid",
    "ukiyo-e woodblock print"
]

@functions_framework.cloud_event
def hello_pubsub(cloud_event):
    # Print out the data from Pub/Sub, to prove that it worked
    print(f"Received Pub/Sub message: {cloud_event.data["message"]["data"]}")

    # 1. Initialize Vertex AI (IAM-based auth, no manual key needed)
    vertexai.init(project=PROJECT_ID, location=REGION)
    genai_client = genai.Client(vertexai=True)
    
    # Example: fetching a secret if you had one (e.g. external service key)
    # api_key = get_secret("SOME_API_KEY") 

    # 2. Get Random Image from GCS
    bucket = storage_client.bucket(BUCKET_NAME)
    blobs = list(bucket.list_blobs())
    if not blobs:
        return "No photos found in bucket", 404
    
    print(blobs)
    
    # Pick image and download
    blob = random.choice(blobs)
    img_bytes = blob.download_as_bytes()

    # Convert downloaded bytes → Pillow image
    ref_image = PILImage.open(io.BytesIO(img_bytes)).convert("RGB")

    # Pick a random style prompt
    style = random.choice(PROMPTS)
    prompt = f"Recreate this photo in the style of {style}. High quality, artistic, highly detailed."
    print(f"Generating with Gemini 3 Pro: {prompt}")

    # Generate content
    response = genai_client.models.generate_content(
        model="gemini-3-pro-image-preview",
        contents=[
            prompt,
            ref_image,        # The image you downloaded
        ],
        config=genai.types.GenerateContentConfig(
            response_modalities=["TEXT", "IMAGE"],
            image_config=genai.types.ImageConfig(
                aspect_ratio="16:9",   # or "4:3" etc.
                image_size="1K"       # "1K", "2K", "4K"
            ),
        ),
    )

    # Save generated output(s)
    for part in response.parts:
        if part.text:
            print("Text output:", part.text)
        elif (image := part.as_image()):
            image.save("output_generated.jpg")
            print("Image generated and saved as output_generated.jpg")

    # # Need to create this into the types.Image and create a ReferenceImage type
    # img_obj = genai.types.Image(image_bytes=img_bytes, mime_type="image/jpeg")
    # ref_image = genai.types.RawReferenceImage(
    #     reference_id=1,
    #     reference_image=img_obj
    # )
    # print(f"Downloaded image: {blob.name}")
    # # 3. Generate Styled Version with Imagen (Vertex AI Vision)
    # style = random.choice(PROMPTS)
    # prompt = f"Recreate this photo in the style of {style}. High quality, artistic, highly detailed."
    # print(f"Generating: {prompt}")
    # # Need to use specific model that can support the edit_image function
    # image_resp = genai_client.models.edit_image(
    #     model="imagen-3.0-capability-001",
    #     prompt=prompt,
    #     reference_images=[ref_image],
    #     config=genai.types.EditImageConfig(
    #         edit_mode='EDIT_MODE_DEFAULT',
    #         number_of_images=1
    #     )
    # )
    # # Save the generated image as a local file
    # gen_image_bytes = image_resp.generated_images[0].image.save("output_generated.jpg")
    # print("Image generated and saved as output_generated.jpg")
    
    
    # Note: GeneratedImage._image_bytes is the most direct way to get bytes in Cloud Functions
    # without writing to a local file system first.
    # output_blob.upload_from_string(generated_image._image_bytes, content_type="image/jpeg")
    
    # public_url = f"https://storage.googleapis.com/{BUCKET_NAME}/{new_filename}"
    
    # # 6. Publish Notification to Pub/Sub
    # topic_path = publisher.topic_path(PROJECT_ID, TOPIC_ID)
    # message_data = json.dumps({
    #     "image_url": public_url,
    #     "style": style,
    #     "original_desc": description
    # }).encode("utf-8")
    
    # future = publisher.publish(topic_path, message_data)
    # message_id = future.result()
    
    # return f"Processed {blob.name} -> {style}. Pub/Sub ID: {message_id}"

    return "OK"
